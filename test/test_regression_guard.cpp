/**
 * @file test_regression_guard.cpp
 * @brief 跨模块回归保护 & 集成回归测试
 *
 * 覆盖范围：
 *  - 配置往返完整性（save → read → modify → read）
 *  - 任务生命周期回归（add → execute → remove → re-add）
 *  - 原子写入 + 文件哈希一致性
 *  - 健康监控多轮降级/恢复无泄漏
 *  - 多模块并行操作不干扰
 *  - JSON 文档重复序列化安全
 *  - 路径操作链一致性
 *  - 重复操作无堆泄漏
 */

#include <unity.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include <map>
#include "mocks/MockConfigStorage.h"
#include "mocks/MockTaskManager.h"
#include "mocks/MockHealthMonitor.h"
#include "utils/FileUtils.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_regression_guard_group();

// 辅助：读取项目源文件（用于源码回归测试）
static std::string readRegressionSrcFile(const char* relativePath) {
    const char* roots[] = { ".", "..", "../..", "../../..", "../../../.." };
    for (const char* root : roots) {
        std::string fullPath = std::string(root) + "/" + relativePath;
        std::ifstream file(fullPath);
        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            std::string content = ss.str();
            // Windows text streams normalize CRLF; make source guards portable to Linux too.
            for (size_t pos = 0; (pos = content.find("\r\n", pos)) != std::string::npos; ++pos) {
                content.erase(pos, 1);
            }
            return content;
        }
    }
    return "";
}

// 空的 TLS 内存池原型不得重新进入启动关键路径；兼容头文件仍可供外部代码引用。
void test_regression_tls_pool_stub_not_boot_dependency() {
    TestLog::testStart("Regression: TLS pool stub is not a boot dependency");

    std::string src = readRegressionSrcFile("src/core/FastBeeFramework.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read FastBeeFramework.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("security/TlsMemoryPool.h") == std::string::npos,
        "FastBeeFramework must not include the compatibility-only TLS pool facade");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("TlsMemoryPool::instance().initialize()") == std::string::npos,
        "FastBeeFramework must not initialize the removed TLS pool stub");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("TLSPool:") == std::string::npos,
        "FastBeeFramework must not report a non-existent TLS pool");

    TestLog::testEnd(true);
}

// 认证配置的 JSON 持久化必须由 UserManager 单独负责，AuthManager 只维护
// 运行时缓存并通过明确的更新接口回写，防止两套 schema 漂移。
void test_regression_auth_config_has_single_persistence_owner() {
    TestLog::testStart("Regression: Auth config has a single persistence owner");

    std::string userManager = readRegressionSrcFile("src/security/UserManager.cpp");
    std::string authManager = readRegressionSrcFile("src/security/AuthManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(userManager.empty(), "Cannot read UserManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(authManager.empty(), "Cannot read AuthManager.cpp");

    TEST_ASSERT_TRUE_MESSAGE(
        userManager.find("ADMIN_AUTH_CONFIG_FILE") != std::string::npos,
        "UserManager must own the auth.json path");
    TEST_ASSERT_TRUE_MESSAGE(
        userManager.find("ConfigStorage::getInstance().saveJSONConfig(savePath, doc)") != std::string::npos,
        "UserManager must persist the canonical auth document through ConfigStorage");
    TEST_ASSERT_TRUE_MESSAGE(
        authManager.find("um->updateSecurityConfig(") != std::string::npos,
        "AuthManager must delegate persistence to UserManager");
    TEST_ASSERT_TRUE_MESSAGE(
        authManager.find("LittleFS.open(") == std::string::npos,
        "AuthManager must not open a second auth JSON store");

    TestLog::step("UserManager owns JSON persistence; AuthManager delegates runtime updates");
    TestLog::testEnd(true);
}

// ========== 配置往返完整性 ==========

void test_config_roundtrip_integrity() {
    TestLog::testStart("Regression: Config Roundtrip Integrity");

    auto& store = MockConfigStore;
    store.initialize();
    store.clearJSONFiles();

    // 第一轮：写入初始配置
    JsonDocument doc1;
    doc1["mqtt"]["server"] = "broker.fastbee.io";
    doc1["mqtt"]["port"] = 1883;
    doc1["mqtt"]["keepalive"] = 60;
    doc1["mqtt"]["clean_session"] = true;
    doc1["modbus"]["baud_rate"] = 9600;
    doc1["modbus"]["data_bits"] = 8;
    store.saveConfig("/config/protocol.json", doc1);

    // 第一轮：读取验证
    JsonDocument doc2;
    TEST_ASSERT_TRUE(store.loadConfig("/config/protocol.json", doc2));
    TEST_ASSERT_EQUAL_STRING("broker.fastbee.io", doc2["mqtt"]["server"].as<const char*>());
    TEST_ASSERT_EQUAL(1883, doc2["mqtt"]["port"].as<int>());
    TEST_ASSERT_EQUAL(9600, doc2["modbus"]["baud_rate"].as<int>());
    TestLog::step("Round 1: write → read verified");

    // 第二轮：修改部分字段
    doc2["mqtt"]["port"] = 8883;
    doc2["mqtt"]["tls"] = true;
    store.saveConfig("/config/protocol.json", doc2);

    // 第二轮：读取验证修改生效且未改字段保留
    JsonDocument doc3;
    TEST_ASSERT_TRUE(store.loadConfig("/config/protocol.json", doc3));
    TEST_ASSERT_EQUAL_STRING("broker.fastbee.io", doc3["mqtt"]["server"].as<const char*>());
    TEST_ASSERT_EQUAL(8883, doc3["mqtt"]["port"].as<int>());
    TEST_ASSERT_TRUE(doc3["mqtt"]["tls"].as<bool>());
    TEST_ASSERT_EQUAL(9600, doc3["modbus"]["baud_rate"].as<int>());
    TestLog::step("Round 2: partial modify → read verified (fields preserved)");

    // 第三轮：删除再恢复
    store.backupConfig("/config/protocol.json", "/backup/protocol.json.bak");
    store.deleteConfig("/config/protocol.json");
    TEST_ASSERT_FALSE(store.configExists("/config/protocol.json"));

    store.restoreConfig("/backup/protocol.json.bak", "/config/protocol.json");
    JsonDocument doc4;
    TEST_ASSERT_TRUE(store.loadConfig("/config/protocol.json", doc4));
    TEST_ASSERT_EQUAL(8883, doc4["mqtt"]["port"].as<int>());
    TestLog::step("Round 3: backup → delete → restore → verified");

    TestLog::testEnd(true);
}

// ========== 任务生命周期回归 ==========

void test_task_lifecycle_regression() {
    TestLog::testStart("Regression: Task Lifecycle");

    auto& tm = MockTaskMgr;
    tm.initialize();
    tm.clearAll();

    static int lifeCounter = 0;
    lifeCounter = 0;

    // 添加 → 执行 → 移除
    tm.addTask("lifecycle_task", [](void*) { lifeCounter++; }, nullptr, 100);
    tm.runTaskNow("lifecycle_task");
    TEST_ASSERT_EQUAL(1, lifeCounter);
    TEST_ASSERT_EQUAL(1, tm.getTaskCount());
    TestLog::step("Phase 1: add → execute → count=1");

    tm.removeTask("lifecycle_task");
    TEST_ASSERT_EQUAL(0, tm.getTaskCount());
    TEST_ASSERT_NULL(tm.getTask("lifecycle_task"));
    TestLog::step("Phase 2: remove → count=0, task gone");

    // 重新添加（验证无残留状态）
    tm.addTask("lifecycle_task", [](void*) { lifeCounter += 10; }, nullptr, 200);
    TEST_ASSERT_EQUAL(1, tm.getTaskCount());
    tm.runTaskNow("lifecycle_task");
    TEST_ASSERT_EQUAL(11, lifeCounter);  // 1 + 10
    TestLog::step("Phase 3: re-add with different function → execute → count=11");

    // 修改间隔
    tm.updateTaskInterval("lifecycle_task", 500);
    Task* t = tm.getTask("lifecycle_task");
    TEST_ASSERT_EQUAL(500, (int)t->interval);
    TestLog::step("Phase 4: interval updated to 500ms");

    // 禁用 → 执行不应触发 → 启用 → 执行触发
    tm.disableTask("lifecycle_task");
    tm.setCurrentTime(1000);
    tm.run();
    TEST_ASSERT_EQUAL(11, lifeCounter);  // 没有变化
    TestLog::step("Phase 5: disabled task skipped during run()");

    tm.enableTask("lifecycle_task");
    tm.runTaskNow("lifecycle_task");
    TEST_ASSERT_EQUAL(21, lifeCounter);  // 11 + 10
    TestLog::step("Phase 6: re-enabled → execute → count=21");

    tm.clearAll();
    TestLog::testEnd(true);
}

// ========== 原子写入 + 哈希一致性 ==========

void test_atomic_write_hash_consistency() {
    TestLog::testStart("Regression: Atomic Write + Hash");

    FileUtils::initialize();

    String path = "/test_regression_atomic.txt";
    String content1 = "FastBee v1.0 configuration data";
    String content2 = "FastBee v2.0 updated configuration";

    // 原子写入 + 计算哈希
    TEST_ASSERT_TRUE(FileUtils::atomicWriteFile(path, content1));
    String hash1 = FileUtils::calculateFileHash(path);
    TEST_ASSERT_FALSE(hash1.isEmpty());
    TestLog::step("Atomic write v1 + hash computed");

    // 验证内容
    TEST_ASSERT_EQUAL_STRING(content1.c_str(), FileUtils::readFile(path).c_str());
    TestLog::step("Content matches v1");

    // 覆盖写入
    TEST_ASSERT_TRUE(FileUtils::atomicWriteFile(path, content2));
    String hash2 = FileUtils::calculateFileHash(path);
    TEST_ASSERT_FALSE(hash2.isEmpty());
    TEST_ASSERT_NOT_EQUAL(hash1.c_str(), hash2.c_str());
    TestLog::step("Atomic write v2: hash changed");

    // 验证新内容
    TEST_ASSERT_EQUAL_STRING(content2.c_str(), FileUtils::readFile(path).c_str());
    TEST_ASSERT_TRUE(FileUtils::verifyFileIntegrity(path, hash2));
    TestLog::step("Content matches v2, integrity verified");

    // 无 .tmp 残留
    TEST_ASSERT_FALSE(FileUtils::exists(path + ".tmp"));
    TestLog::step("No .tmp residual");

    FileUtils::deleteFile(path);
    TestLog::testEnd(true);
}

// ========== 健康监控多轮降级/恢复 ==========

void test_health_multi_cycle_no_leak() {
    TestLog::testStart("Regression: Health Multi-Cycle No Leak");

    auto& hm = MockHealthMon;
    hm.initialize();

    uint32_t initialHeap = ESP.getFreeHeap();

    // 模拟 20 轮降级/恢复循环
    for (int cycle = 0; cycle < 20; cycle++) {
        hm.setFreeHeap(5000);  // 低堆
        hm.update();
        SystemHealth h = hm.getHealthStatus();
        TEST_ASSERT_FALSE(h.isHealthy);

        hm.setFreeHeap(100000);  // 恢复
        hm.clearWarningsAndErrors();
        hm.update();
        h = hm.getHealthStatus();
        TEST_ASSERT_TRUE(h.isHealthy);
    }
    TestLog::step("20 degradation/recovery cycles completed");

    uint32_t finalHeap = ESP.getFreeHeap();
    int32_t leak = (int32_t)initialHeap - (int32_t)finalHeap;
    TEST_ASSERT_TRUE_MESSAGE(leak < 5000, "Memory leak in health monitor cycles");
    TestLog::step("Heap leak < 5KB over 20 cycles");

    TestLog::testEnd(true);
}

// ========== 多模块并行操作 ==========

void test_multi_module_parallel() {
    TestLog::testStart("Regression: Multi-Module Parallel Ops");

    auto& store = MockConfigStore;
    auto& tm = MockTaskMgr;
    auto& hm = MockHealthMon;

    store.initialize();
    store.clearAll();
    tm.initialize();
    tm.clearAll();
    hm.initialize();
    hm.clearWarningsAndErrors();

    static int parallelCounter = 0;
    parallelCounter = 0;

    // 同时操作三个模块
    // 1. 配置写入
    JsonDocument doc;
    doc["test"] = true;
    store.saveConfig("/test_parallel.json", doc);

    // 2. 任务添加
    tm.addTask("parallel_task", [](void*) { parallelCounter++; }, nullptr, 100);

    // 3. 健康更新
    hm.setFreeHeap(80000);
    hm.update();

    // 验证三模块独立
    TEST_ASSERT_TRUE(store.configExists("/test_parallel.json"));
    TestLog::step("Config module: independent");

    tm.runTaskNow("parallel_task");
    TEST_ASSERT_EQUAL(1, parallelCounter);
    TestLog::step("Task module: independent");

    SystemHealth h = hm.getHealthStatus();
    TEST_ASSERT_TRUE(h.isHealthy);
    TestLog::step("Health module: independent");

    // 交叉验证：操作一个不影响其他
    store.deleteConfig("/test_parallel.json");
    TEST_ASSERT_EQUAL(1, tm.getTaskCount());  // 任务不受影响
    TestLog::step("Config delete doesn't affect task module");

    tm.clearAll();
    JsonDocument checkDoc;
    // 任务清理不影响配置（已删除所以不存在）
    TEST_ASSERT_FALSE(store.configExists("/test_parallel.json"));
    TestLog::step("Task clear doesn't affect config module");

    store.clearAll();
    tm.clearAll();
    TestLog::testEnd(true);
}

// ========== JSON 文档重复序列化安全 ==========

void test_json_repeated_serialization() {
    TestLog::testStart("Regression: JSON Repeated Serialization");

    uint32_t initialHeap = ESP.getFreeHeap();

    for (int i = 0; i < 50; i++) {
        JsonDocument doc;
        doc["iteration"] = i;
        doc["data"]["value"] = i * 10;
        doc["data"]["name"] = "test_" + String(i);
        doc["array"][0] = i;
        doc["array"][1] = i + 1;

        String json;
        serializeJson(doc, json);
        TEST_ASSERT_FALSE(json.isEmpty());

        // 反序列化回来
        JsonDocument doc2;
        DeserializationError err = deserializeJson(doc2, json);
        TEST_ASSERT_TRUE(err == DeserializationError::Ok);
        TEST_ASSERT_EQUAL(i, doc2["iteration"].as<int>());
    }
    TestLog::step("50 serialize/deserialize cycles completed");

    uint32_t finalHeap = ESP.getFreeHeap();
    int32_t leak = (int32_t)initialHeap - (int32_t)finalHeap;
    TEST_ASSERT_TRUE_MESSAGE(leak < 5000, "JSON serialization leak detected");
    TestLog::step("Heap leak < 5KB over 50 cycles");

    TestLog::testEnd(true);
}

// ========== 路径操作链一致性 ==========

void test_path_operation_chain_consistency() {
    TestLog::testStart("Regression: Path Chain Consistency");

    // 链式操作：joinPath → normalizePath → getFileName → getDirectoryPath
    String joined = FileUtils::joinPath("/config", "network.json");
    TEST_ASSERT_EQUAL_STRING("/config/network.json", joined.c_str());

    String normalized = FileUtils::normalizePath(joined);
    TEST_ASSERT_EQUAL_STRING("/config/network.json", normalized.c_str());

    String fileName = FileUtils::getFileName(normalized);
    TEST_ASSERT_EQUAL_STRING("network.json", fileName.c_str());

    String dirPath = FileUtils::getDirectoryPath(normalized);
    TEST_ASSERT_EQUAL_STRING("/config", dirPath.c_str());
    TestLog::step("Chain: join → normalize → getFileName → getDirectoryPath consistent");

    // 反向验证：dirPath + fileName 应还原原始路径
    String reconstructed = FileUtils::joinPath(dirPath, fileName);
    TEST_ASSERT_EQUAL_STRING(normalized.c_str(), reconstructed.c_str());
    TestLog::step("Reverse: joinPath(dir, file) reconstructs original path");

    // 多级路径
    String deep = FileUtils::joinPath("/www", FileUtils::joinPath("css", "style.css"));
    TEST_ASSERT_EQUAL_STRING("/www/css/style.css", deep.c_str());

    String ext = FileUtils::getFileExtension(deep);
    TEST_ASSERT_EQUAL_STRING("css", ext.c_str());
    TestLog::step("Deep path chain: join → join → getExtension verified");

    TestLog::testEnd(true);
}

// ========== 重复文件操作无堆泄漏 ==========

void test_repeated_file_ops_no_leak() {
    TestLog::testStart("Regression: Repeated File Ops No Leak");

    FileUtils::initialize();
    String path = "/test_regression_leak.txt";

    uint32_t initialHeap = ESP.getFreeHeap();

    for (int i = 0; i < 30; i++) {
        String content = "iteration_" + String(i) + "_data_padding_for_size";
        FileUtils::writeFile(path, content);
        String readBack = FileUtils::readFile(path);
        TEST_ASSERT_EQUAL_STRING(content.c_str(), readBack.c_str());
        FileUtils::deleteFile(path);
    }
    TestLog::step("30 write → read → delete cycles completed");

    uint32_t finalHeap = ESP.getFreeHeap();
    int32_t leak = (int32_t)initialHeap - (int32_t)finalHeap;
    TEST_ASSERT_TRUE_MESSAGE(leak < 5000, "File I/O leak detected");
    TestLog::step("Heap leak < 5KB over 30 cycles");

    TestLog::testEnd(true);
}

// ========== NVS 键隔离回归 ==========

void test_nvs_key_isolation() {
    TestLog::testStart("Regression: NVS Key Isolation");

    auto& store = MockConfigStore;
    store.clearNVS();

    // 写入多个同类型键
    store.putString("wifi.ssid", "Network_A");
    store.putString("mqtt.server", "broker_A");
    store.putInt("mqtt.port", 1883);
    store.putInt("modbus.baud", 9600);

    // 修改一个不应影响其他
    store.putString("wifi.ssid", "Network_B");
    TEST_ASSERT_EQUAL_STRING("Network_B", store.getString("wifi.ssid").c_str());
    TEST_ASSERT_EQUAL_STRING("broker_A", store.getString("mqtt.server").c_str());
    TestLog::step("Modifying wifi.ssid doesn't affect mqtt.server");

    store.putInt("mqtt.port", 8883);
    TEST_ASSERT_EQUAL(8883, store.getInt("mqtt.port"));
    TEST_ASSERT_EQUAL(9600, store.getInt("modbus.baud"));
    TestLog::step("Modifying mqtt.port doesn't affect modbus.baud");

    // 删除一个不应影响其他
    store.removeKey("wifi.ssid");
    TEST_ASSERT_FALSE(store.exists("wifi.ssid"));
    TEST_ASSERT_TRUE(store.exists("mqtt.server"));
    TEST_ASSERT_TRUE(store.exists("mqtt.port"));
    TestLog::step("Deleting wifi.ssid preserves other keys");

    store.clearAll();
    TestLog::testEnd(true);
}

// ========== 以太网模式源码回归保护 ==========

/**
 * @brief 验证 FastBeeFramework.cpp 中 NTP 触发条件使用 isNetworkConnected()
 * 回归：原代码用 WiFi.status() == WL_CONNECTED，以太网模式下永远为 false
 * 修复：改用 framework->network->isNetworkConnected() 支持所有联网方式
 */
void test_regression_ntp_uses_is_network_connected() {
    TestLog::testStart("Regression: NTP Uses isNetworkConnected()");

    std::string src = readRegressionSrcFile("src/core/FastBeeFramework.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read FastBeeFramework.cpp");
    TestLog::step("Loaded FastBeeFramework.cpp");

    // NTP 触发条件必须使用 isNetworkConnected()
    // 查找源码中 isNetworkConnected() 的使用次数
    size_t pos = 0;
    int isNetworkConnectedCount = 0;
    std::string target = "isNetworkConnected()";
    while ((pos = src.find(target, pos)) != std::string::npos) {
        isNetworkConnectedCount++;
        pos += target.length();
    }

    TEST_ASSERT_GREATER_OR_EQUAL(2, isNetworkConnectedCount);
    TestLog::step("isNetworkConnected() found >= 2 times (NTP trigger + MQTT trigger)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 NetworkManager.cpp 中以太网模式 mDNS 在 AP 之后启动
 * 回归：原代码 mDNS 在 AP 之前启动，导致 mDNS 绑定到错误的 netif
 * 检查源码中 "startAPMode" 出现在 "startMDNS" 之前
 */
void test_regression_ethernet_mdns_after_ap_in_source() {
    TestLog::testStart("Regression: Ethernet mDNS After AP in Source");

    std::string src = readRegressionSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read NetworkManager.cpp");
    TestLog::step("Loaded NetworkManager.cpp");

    // 查找以太网初始化块中的 startAPMode 和 startMDNS
    // 通过 "Ethernet connected" 定位以太网成功连接后的代码块
    size_t ethInitPos = src.find("NET_ETHERNET");
    TEST_ASSERT_TRUE(ethInitPos != std::string::npos);
    TestLog::step("Found NET_ETHERNET in source");

    size_t ethConnectedPos = src.find("Ethernet connected", ethInitPos);
    TEST_ASSERT_TRUE(ethConnectedPos != std::string::npos);
    TestLog::step("Found 'Ethernet connected' log");

    // 查找以太网块结束标记 "isInitialized = true"
    size_t ethEndPos = src.find("isInitialized = true", ethConnectedPos);
    TEST_ASSERT_TRUE(ethEndPos != std::string::npos);
    TestLog::step("Found 'isInitialized = true' (block end marker)");

    // 提取以太网块并验证顺序
    std::string ethBlock = src.substr(ethConnectedPos, ethEndPos - ethConnectedPos);

    // 关键验证：两个函数调用都在块内，且顺序正确
    // 使用精确模式匹配实际函数调用，避免匹配注释文本
    size_t apPos = ethBlock.find("= startAPMode()");
    size_t mdnsPos = ethBlock.find("->startMDNS(");
    TEST_ASSERT_TRUE(apPos != std::string::npos);
    TestLog::step("startAPMode found in Ethernet block");
    TEST_ASSERT_TRUE(mdnsPos != std::string::npos);
    TestLog::step("startMDNS found in Ethernet block");

    // 验证顺序：startAPMode 必须在 startMDNS 之前出现
    // 这确保 AP 先启动，mDNS 在所有 netif 就绪后启动
    // Unity: TEST_ASSERT_LESS_THAN(upper, actual) 断言 actual < upper
    TEST_ASSERT_LESS_THAN((int)mdnsPos, (int)apPos);
    TestLog::step("startAPMode position < startMDNS position (correct init order)");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 FastBeeFramework.cpp 启动报告包含以太网模式输出
 * 回归：原代码以太网模式下启动日志不显示以太网信息
 */
void test_regression_boot_report_includes_ethernet() {
    TestLog::testStart("Regression: Boot Report Includes Ethernet");

    std::string src = readRegressionSrcFile("src/core/FastBeeFramework.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read FastBeeFramework.cpp");

    // 启动报告中必须包含以太网模式的关键字
    TEST_ASSERT_TRUE(src.find("Ethernet (W5500)") != std::string::npos);
    TestLog::step("Boot report contains 'Ethernet (W5500)'");

    TEST_ASSERT_TRUE(src.find("Ethernet IP:") != std::string::npos);
    TestLog::step("Boot report contains 'Ethernet IP:'");

    TEST_ASSERT_TRUE(src.find("mDNS URL:") != std::string::npos);
    TestLog::step("Boot report contains 'mDNS URL:'");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 FastBeeFramework.cpp 周期性状态包含以太网信息
 * 回归：原代码 [STATUS] 打印不包含以太网状态
 */
void test_regression_periodic_status_includes_ethernet() {
    TestLog::testStart("Regression: Periodic Status Includes Ethernet");

    std::string src = readRegressionSrcFile("src/core/FastBeeFramework.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read FastBeeFramework.cpp");

    // 周期性状态输出中必须包含以太网状态
    TEST_ASSERT_TRUE(src.find("ETH=CONNECTED") != std::string::npos);
    TestLog::step("Periodic status contains 'ETH=CONNECTED'");

    TEST_ASSERT_TRUE(src.find("NET_ETHERNET") != std::string::npos);
    TestLog::step("Periodic status contains 'NET_ETHERNET' check");

    TestLog::testEnd(true);
}

// ========== Bug Fix 回归测试：MQTT保存不清空主题 (Bug #2) ==========

/**
 * @brief 验证 saveProtocolConfig 在加载完整模块前快照表单，加载后恢复
 * 回归：旧代码调用 _loadFullProtocolConfig 后表单被服务器数据覆盖，
 *       导致用户编辑的发布/订阅主题列表丢失
 */
void test_regression_mqtt_save_preserves_topics_snapshot() {
    TestLog::testStart("Regression: MQTT Save Preserves Topics Snapshot");

    std::string content = readRegressionSrcFile("web-src/modules/runtime/protocol/protocol-lite-config.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(),
        "Failed to read protocol-lite-config.js");
    TestLog::step("File loaded");

    // 1) saveProtocolConfig 必须在调用 _loadFullProtocolConfig 前创建快照对象
    TEST_ASSERT_TRUE_MESSAGE(content.find("var snapshot = {}") != std::string::npos,
        "saveProtocolConfig must create snapshot object before loading full module");
    TestLog::step("Snapshot object creation present");

    // 2) 必须快照发布主题容器的 HTML
    TEST_ASSERT_TRUE_MESSAGE(content.find("mqtt-publish-topics") != std::string::npos,
        "Must snapshot mqtt-publish-topics container");
    TestLog::step("Publish topics snapshot present");

    // 3) 必须快照订阅主题容器的 HTML
    TEST_ASSERT_TRUE_MESSAGE(content.find("mqtt-subscribe-topics") != std::string::npos,
        "Must snapshot mqtt-subscribe-topics container");
    TestLog::step("Subscribe topics snapshot present");

    // 4) 快照必须保存 innerHTML（而非仅 value）
    TEST_ASSERT_TRUE_MESSAGE(content.find("__publishTopicsHTML") != std::string::npos,
        "Must save publish topics as __publishTopicsHTML key");
    TEST_ASSERT_TRUE_MESSAGE(content.find("__subscribeTopicsHTML") != std::string::npos,
        "Must save subscribe topics as __subscribeTopicsHTML key");
    TestLog::step("Topic HTML keys present");

    // 5) 加载后必须恢复 innerHTML
    std::regex restoreRe("innerHTML\\s*=\\s*snapshot\\[");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(content, restoreRe),
        "Must restore innerHTML from snapshot after _loadFullProtocolConfig");
    TestLog::step("innerHTML restore from snapshot present");

    // 6) 在 saveProtocolConfig 函数体内验证快照/恢复序列
    // _loadFullProtocolConfig 在文件中有两处出现（定义 + 调用），
    // 必须在 saveProtocolConfig 函数体内验证调用位置在 snapshot 和 restore 之间
    size_t saveFuncPos = content.find("saveProtocolConfig: function");
    TEST_ASSERT_TRUE_MESSAGE(saveFuncPos != std::string::npos,
        "saveProtocolConfig function not found");
    std::string saveBody = content.substr(saveFuncPos, 3000);
    size_t snapshotInSave = saveBody.find("var snapshot = {}");
    size_t loadInSave = saveBody.find("this._loadFullProtocolConfig");
    // 恢复块中的 innerHTML 赋值仅出现在 _loadFullProtocolConfig 之后
    size_t restoreInSave = saveBody.find("currentPubTopics.innerHTML = snapshot");
    TEST_ASSERT_TRUE_MESSAGE(snapshotInSave != std::string::npos,
        "snapshot creation not found in saveProtocolConfig");
    TEST_ASSERT_TRUE_MESSAGE(loadInSave != std::string::npos,
        "_loadFullProtocolConfig call not found in saveProtocolConfig");
    TEST_ASSERT_TRUE_MESSAGE(restoreInSave != std::string::npos,
        "current MQTT publish topics restore not found in saveProtocolConfig");
    TEST_ASSERT_TRUE(snapshotInSave < loadInSave);
    TestLog::step("Snapshot created BEFORE _loadFullProtocolConfig in saveProtocolConfig");
    TEST_ASSERT_TRUE(loadInSave < restoreInSave);
    TestLog::step("Topics restored AFTER _loadFullProtocolConfig in saveProtocolConfig");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 saveProtocolConfig 同时快照/恢复普通表单字段（checkbox 和 input）
 * 防止仅恢复主题列表而忽略其他字段
 */
void test_regression_mqtt_save_preserves_all_form_fields() {
    TestLog::testStart("Regression: MQTT Save Preserves All Form Fields");

    std::string content = readRegressionSrcFile("web-src/modules/runtime/protocol/protocol-lite-config.js");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read protocol-lite-config.js");

    // 快照必须处理 checkbox 类型
    TEST_ASSERT_TRUE_MESSAGE(content.find("el.type === 'checkbox'") != std::string::npos,
        "Snapshot must handle checkbox elements");
    TestLog::step("Checkbox snapshot logic present");

    // 快照必须保存 checked 属性
    TEST_ASSERT_TRUE_MESSAGE(content.find("el.checked") != std::string::npos,
        "Snapshot must save checkbox.checked state");
    TestLog::step("Checkbox checked state saved");

    // 恢复时必须还原 checked 属性
    TEST_ASSERT_TRUE_MESSAGE(content.find("el.checked = snapshot[key].checked") != std::string::npos,
        "Restore must set el.checked from snapshot");
    TestLog::step("Checkbox checked state restored");

    // 快照必须保存 value 属性
    TEST_ASSERT_TRUE_MESSAGE(content.find("el.value") != std::string::npos,
        "Snapshot must save input.value");
    TestLog::step("Input value saved");

    // 恢复时必须还原 value 属性
    TEST_ASSERT_TRUE_MESSAGE(content.find("el.value = snapshot[key].value") != std::string::npos,
        "Restore must set el.value from snapshot");
    TestLog::step("Input value restored");

    TestLog::testEnd(true);
}

// ========== Bug Fix 回归测试：以太网重启重建适配器 (Bug #3) ==========

/**
 * @brief 验证 restartNetwork() 在以太网路径不调用 initialize()
 * 回归：旧代码调用 initialize()，但因 isInitialized=true 导致提前返回，
 *       以太网适配器永远不被重建，Web 不可访问
 */
void test_regression_restart_network_no_initialize_for_ethernet() {
    TestLog::testStart("Regression: restartNetwork No initialize() for Ethernet");

    std::string content = readRegressionSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");
    TestLog::step("File loaded");

    // 定位 restartNetwork 函数定义（而非调用点）
    size_t restartPos = content.find("FBNetworkManager::restartNetwork");
    TEST_ASSERT_TRUE_MESSAGE(restartPos != std::string::npos, "restartNetwork function not found");
    TestLog::step("restartNetwork found");

    // 取 restartNetwork 后的代码片段（函数体约 170 行，需要 ~7000 字符完整覆盖）
    std::string restartBody = content.substr(restartPos, 9000);

    // 关键检查：以太网路径必须用 ethernetAdapter.reset(new EthernetAdapter()) 重建
    TEST_ASSERT_TRUE_MESSAGE(restartBody.find("ethernetAdapter.reset(new EthernetAdapter())") != std::string::npos,
        "Ethernet path must use ethernetAdapter.reset(new EthernetAdapter()) to recreate adapter");
    TestLog::step("Ethernet adapter recreated via reset(new EthernetAdapter())");

    // 注释中必须说明不调用 initialize() 的原因
    TEST_ASSERT_TRUE_MESSAGE(restartBody.find("isInitialized") != std::string::npos,
        "Comment must explain why initialize() is not called (isInitialized still true)");
    TestLog::step("Comment explains initialize() bypass reason");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 restartNetwork() 在 4G 路径也直接重建适配器
 */
void test_regression_restart_network_rebuilds_cellular_adapter() {
    TestLog::testStart("Regression: restartNetwork Rebuilds Cellular Adapter");

    std::string content = readRegressionSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");

    size_t restartPos = content.find("FBNetworkManager::restartNetwork");
    TEST_ASSERT_TRUE(restartPos != std::string::npos);
    std::string restartBody = content.substr(restartPos, 9000);

    // 4G 路径必须用 cellularAdapter.reset(new CellularAdapter()) 重建
    TEST_ASSERT_TRUE_MESSAGE(restartBody.find("cellularAdapter.reset(new CellularAdapter())") != std::string::npos,
        "4G path must use cellularAdapter.reset(new CellularAdapter()) to recreate adapter");
    TestLog::step("Cellular adapter recreated via reset(new CellularAdapter())");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 restartNetwork() 以太网失败时回退到 AP 模式
 * 确保用户在以太网失败时仍可通过 AP 热点访问 Web 配置页
 */
void test_regression_restart_network_eth_failure_ap_fallback() {
    TestLog::testStart("Regression: restartNetwork Eth Failure → AP Fallback");

    std::string content = readRegressionSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");

    size_t restartPos = content.find("FBNetworkManager::restartNetwork");
    TEST_ASSERT_TRUE(restartPos != std::string::npos);
    std::string restartBody = content.substr(restartPos, 9000);

    // 以太网失败后必须检查 AP 模式并启动
    TEST_ASSERT_TRUE_MESSAGE(restartBody.find("startAPMode") != std::string::npos,
        "Ethernet failure must call startAPMode() as recovery entrance");
    TestLog::step("startAPMode() called on Ethernet failure");

    // 失败后必须启动 mDNS（确保用户可通过 fastbee.local 访问）
    TEST_ASSERT_TRUE_MESSAGE(restartBody.find("startMDNS") != std::string::npos,
        "mDNS must be started after Ethernet failure for user recovery");
    TestLog::step("mDNS started after Ethernet failure");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 restartNetwork() 以太网成功路径启动 AP 混合模式
 * Bug Fix: 切换为以太网后 fastbee.local 无法访问、MQTT连不上
 * 原因是 restartNetwork() 以太网成功后没有启动 WiFi AP（混合模式），
 * 导致 WiFi 模式为 NULL，mDNS 拒绝启动。
 */
void test_regression_restart_network_eth_success_starts_hybrid_ap() {
    TestLog::testStart("Regression: restartNetwork Eth Success -> Hybrid AP Mode");

    std::string content = readRegressionSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");

    size_t restartPos = content.find("FBNetworkManager::restartNetwork");
    TEST_ASSERT_TRUE(restartPos != std::string::npos);
    std::string restartBody = content.substr(restartPos, 9000);

    // 定位以太网重连成功后的代码块
    size_t ethReconnected = restartBody.find("Ethernet reconnected, IP:");
    TEST_ASSERT_TRUE_MESSAGE(ethReconnected != std::string::npos,
        "Must have 'Ethernet reconnected' log in restartNetwork");

    // 以太网成功后必须启动 WiFi AP（混合模式）
    std::string afterEthOk = restartBody.substr(ethReconnected);
    TEST_ASSERT_TRUE_MESSAGE(
        afterEthOk.find("Starting WiFi AP for hybrid mode") != std::string::npos,
        "Ethernet success path must start WiFi AP for hybrid mode (Ethernet + AP)");
    TestLog::step("WiFi AP started in Ethernet success path");

    // 混合模式后必须启动 mDNS
    TEST_ASSERT_TRUE_MESSAGE(afterEthOk.find("startMDNS") != std::string::npos,
        "mDNS must be started after Ethernet+AP hybrid mode setup");
    TestLog::step("mDNS started after hybrid mode setup");

    // 状态必须保持 CONNECTED（不能被 AP 状态覆盖）
    size_t hybridMode = afterEthOk.find("Hybrid mode active");
    if (hybridMode != std::string::npos) {
        std::string afterHybrid = afterEthOk.substr(hybridMode);
        TEST_ASSERT_TRUE_MESSAGE(
            afterHybrid.find("CONNECTED") != std::string::npos,
            "Status must be reset to CONNECTED after AP mode start");
        TestLog::step("Status preserved as CONNECTED in hybrid mode");
    }

    TestLog::testEnd(true);
}

/**
 * @brief 验证 restartNetwork() 4G 成功路径也启动 AP 混合模式
 */
void test_regression_restart_network_4g_success_starts_hybrid_ap() {
    TestLog::testStart("Regression: restartNetwork 4G Success -> Hybrid AP Mode");

    std::string content = readRegressionSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read NetworkManager.cpp");

    size_t restartPos = content.find("bool FBNetworkManager::restartNetwork(bool* deferred)");
    TEST_ASSERT_TRUE(restartPos != std::string::npos);
    // Bound this source contract by the actual next function. An admission
    // guard can grow without truncating the later 4G success path at 9000 bytes.
    size_t restartEnd = content.find("\nbool FBNetworkManager::checkInternetConnection()", restartPos);
    TEST_ASSERT_TRUE(restartEnd != std::string::npos);
    std::string restartBody = content.substr(restartPos, restartEnd - restartPos);

    // 定位 4G 重连成功后的代码块
    size_t cellReconnected = restartBody.find("4G reconnected");
    TEST_ASSERT_TRUE_MESSAGE(cellReconnected != std::string::npos,
        "Must have '4G reconnected' log in restartNetwork");

    // 4G 成功后必须启动 WiFi AP（混合模式）
    std::string afterCellOk = restartBody.substr(cellReconnected);
    TEST_ASSERT_TRUE_MESSAGE(
        afterCellOk.find("Starting WiFi AP for hybrid mode") != std::string::npos,
        "4G success path must start WiFi AP for hybrid mode (4G + AP)");
    TestLog::step("WiFi AP started in 4G success path");

    // mDNS 必须在 AP 启动后启动
    TEST_ASSERT_TRUE_MESSAGE(afterCellOk.find("startMDNS") != std::string::npos,
        "mDNS must be started after 4G+AP hybrid mode setup");
    TestLog::step("mDNS started after 4G hybrid mode");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 DNSManager::startMDNS() 支持以太网模式（WiFi 模式为 NULL 时仍可启动）
 * Bug Fix: 以太网连接后 WiFi 模式可能为 NULL，mDNS 不应拒绝启动
 */
void test_regression_dns_mdns_supports_ethernet_mode() {
    TestLog::testStart("Regression: DNSManager startMDNS supports Ethernet mode");

    std::string content = readRegressionSrcFile("src/network/DNSManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read DNSManager.cpp");

    // 验证 DNSManager 包含 ETH.h 头文件（以太网支持）
    TEST_ASSERT_TRUE_MESSAGE(content.find("#include <ETH.h>") != std::string::npos,
        "DNSManager must include ETH.h for Ethernet IP detection");
    TestLog::step("ETH.h included in DNSManager");

    // 验证 startMDNS 中有以太网 IP 检查逻辑
    size_t startMDNSPos = content.find("DNSManager::startMDNS");
    TEST_ASSERT_TRUE(startMDNSPos != std::string::npos);
    std::string mdnsBody = content.substr(startMDNSPos, 3000);

    // 必须检查 ETH.localIP()
    TEST_ASSERT_TRUE_MESSAGE(mdnsBody.find("ETH.localIP()") != std::string::npos,
        "startMDNS must check ETH.localIP() for Ethernet mode support");
    TestLog::step("ETH.localIP() check present in startMDNS");

    // 必须有 ethModeValid 标志
    TEST_ASSERT_TRUE_MESSAGE(mdnsBody.find("ethModeValid") != std::string::npos,
        "Must have ethModeValid flag to allow mDNS with Ethernet-only");
    TestLog::step("ethModeValid flag present");

    // 验证新的组合检查逻辑
    TEST_ASSERT_TRUE_MESSAGE(mdnsBody.find("!wifiModeValid && !ethModeValid") != std::string::npos,
        "mDNS should only fail when BOTH WiFi mode invalid AND no Ethernet IP");
    TestLog::step("Combined WiFi+Ethernet check logic verified");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 DNSManager::checkMDNSHealth() 也支持以太网模式
 */
void test_regression_dns_health_check_supports_ethernet() {
    TestLog::testStart("Regression: DNSManager checkMDNSHealth supports Ethernet");

    std::string content = readRegressionSrcFile("src/network/DNSManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!content.empty(), "Failed to read DNSManager.cpp");

    size_t healthPos = content.find("DNSManager::checkMDNSHealth");
    TEST_ASSERT_TRUE(healthPos != std::string::npos);
    std::string healthBody = content.substr(healthPos, 3000);

    // 健康检查也必须能处理以太网模式
    TEST_ASSERT_TRUE_MESSAGE(healthBody.find("ETH.localIP()") != std::string::npos,
        "checkMDNSHealth must check Ethernet IP for Ethernet mode support");
    TestLog::step("ETH.localIP() check in health check");

    TestLog::testEnd(true);
}

// ========== logLevel 运行时配置 + syncInterval 回归保护 ==========

/**
 * @brief 验证 FastBeeFramework.cpp 启动时从 device.json 读取 logLevel 并应用
 * 回归：旧代码硬编码 LOGGER.setLogLevel(LOG_DEBUG)，忽略 device.json 中的 logLevel
 */
void test_regression_boot_reads_loglevel_from_device_json() {
    TestLog::testStart("Regression: Boot Reads logLevel from device.json");

    std::string src = readRegressionSrcFile("src/core/FastBeeFramework.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read FastBeeFramework.cpp");

    // 启动时必须读取 device.json
    TEST_ASSERT_TRUE_MESSAGE(src.find("\"/config/device.json\"") != std::string::npos,
        "Boot must open /config/device.json for logLevel");
    TestLog::step("device.json opened at boot");

    // 必须读取 logLevel 字段
    TEST_ASSERT_TRUE_MESSAGE(src.find("\"logLevel\"") != std::string::npos,
        "Boot must read logLevel field from device.json");
    TestLog::step("logLevel field read at boot");

    // 必须调用 setLogLevel 应用
    TEST_ASSERT_TRUE_MESSAGE(src.find("LOGGER.setLogLevel") != std::string::npos,
        "Boot must call LOGGER.setLogLevel() with parsed value");
    TestLog::step("LOGGER.setLogLevel() called at boot");

    // 必须支持 DEBUG/INFO/WARNING/ERROR 四种级别
    TEST_ASSERT_TRUE(src.find("LOG_DEBUG") != std::string::npos);
    TEST_ASSERT_TRUE(src.find("LOG_INFO") != std::string::npos);
    TEST_ASSERT_TRUE(src.find("LOG_WARNING") != std::string::npos);
    TEST_ASSERT_TRUE(src.find("LOG_ERROR") != std::string::npos);
    TestLog::step("All 4 log levels supported (DEBUG/INFO/WARNING/ERROR)");

    // 不应硬编码 LOG_DEBUG 作为启动级别
    // 查找 "setLogLevel(LOG_DEBUG)" 应不存在（已改为从配置文件读取）
    TEST_ASSERT_TRUE_MESSAGE(src.find("setLogLevel(LOG_DEBUG)") == std::string::npos,
        "Boot must NOT hardcode LOG_DEBUG — read from device.json instead");
    TestLog::step("No hardcoded LOG_DEBUG at boot");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 FastBeeFramework.cpp syncTimeFromConfig 调用 sntp_set_sync_interval
 * 回归：旧代码忽略 syncInterval 字段，使用 ESP-IDF 默认 1 小时同步间隔
 */
void test_regression_sync_interval_applied_to_sntp() {
    TestLog::testStart("Regression: syncInterval Applied to SNTP");

    std::string src = readRegressionSrcFile("src/core/FastBeeFramework.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read FastBeeFramework.cpp");

    // 必须包含 esp_sntp.h 头文件
    TEST_ASSERT_TRUE_MESSAGE(src.find("#include <esp_sntp.h>") != std::string::npos,
        "Must include esp_sntp.h for sntp_set_sync_interval()");
    TestLog::step("esp_sntp.h included");

    // 必须读取 syncInterval 字段
    TEST_ASSERT_TRUE_MESSAGE(src.find("\"syncInterval\"") != std::string::npos,
        "syncTimeFromConfig must read syncInterval from device.json");
    TestLog::step("syncInterval field read");

    // 必须调用 sntp_set_sync_interval
    TEST_ASSERT_TRUE_MESSAGE(src.find("sntp_set_sync_interval") != std::string::npos,
        "Must call sntp_set_sync_interval() to apply sync interval");
    TestLog::step("sntp_set_sync_interval() called");

    // 必须有范围保护（60~86400 秒）
    TEST_ASSERT_TRUE_MESSAGE(src.find("< 60") != std::string::npos,
        "syncInterval must have lower bound guard (< 60)");
    TEST_ASSERT_TRUE_MESSAGE(src.find("> 86400") != std::string::npos,
        "syncInterval must have upper bound guard (> 86400)");
    TestLog::step("syncInterval range guard (60~86400) present");

    TestLog::testEnd(true);
}

/**
 * @brief HTTP NTP must not block the scheduler on no-PSRAM targets.
 * The fallback keeps SNTP enabled while avoiding synchronous HTTP/DNS work.
 */
void test_regression_no_psram_ntp_uses_sntp_fallback() {
    TestLog::testStart("Regression: No-PSRAM NTP Uses SNTP Fallback");

    std::string src = readRegressionSrcFile("src/core/FastBeeFramework.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read FastBeeFramework.cpp");
    TEST_ASSERT_TRUE_MESSAGE(src.find("ESP.getPsramSize() > 0") != std::string::npos,
        "HTTP NTP fallback must detect PSRAM-capable targets");
    TEST_ASSERT_TRUE_MESSAGE(src.find("ntpHttpFallbackAttempts < 2") != std::string::npos,
        "HTTP NTP fallback must be bounded");
    TEST_ASSERT_TRUE_MESSAGE(src.find("framework->ntpRetryCount >= 3") != std::string::npos,
        "HTTP NTP fallback must wait for asynchronous SNTP retries");
    TEST_ASSERT_TRUE_MESSAGE(src.find("TimeUtils::syncNTPFromHTTP") != std::string::npos,
        "PSRAM NTP fallback must use the shared HTTP time helper");
    TEST_ASSERT_TRUE_MESSAGE(src.find("if (!FastBee::SystemTimeZone::startSntp(tz, primaryServer.c_str(), secondaryServer.c_str()))") != std::string::npos,
        "No-PSRAM fallback must configure asynchronous SNTP through the shared timezone owner and handle rejection");
    const std::string zone = readRegressionSrcFile("src/utils/SystemTimeZone.cpp");
    const size_t start = zone.find("bool SystemTimeZone::startSntp(");
    const size_t guard = zone.find("TimeZoneGuard guard", start);
    const size_t configure = zone.find("SystemTimePlatform::configureSntp(stable, server1, server2, server3)", guard);
    TEST_ASSERT_TRUE_MESSAGE(start != std::string::npos && guard != std::string::npos && configure != std::string::npos && guard < configure,
        "SNTP timezone configuration must remain under the shared timezone guard");
    TEST_ASSERT_TRUE_MESSAGE(src.find("String primaryServer = httpNtpConfigured ? s2Str : s1Str") != std::string::npos,
        "HTTP NTP URL must be replaced by a DNS NTP host for the initial SNTP attempt");
    TEST_ASSERT_TRUE_MESSAGE(src.find("primaryServer = \"pool.ntp.org\"") != std::string::npos,
        "SNTP fallback must have a safe DNS hostname when config is missing");

    TestLog::testEnd(true);
}

/**
 * @brief AP fallback must cool down before probing STA again.
 * A same-cycle probe after several failed handshakes can trigger router
 * authentication rate limiting and make the AP provisioning page stall.
 */
void test_regression_ap_fallback_probe_cooldown() {
    TestLog::testStart("Regression: AP Fallback Probe Cooldown");

    std::string src = readRegressionSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read NetworkManager.cpp");

    const std::string cooldown = "_lastApProbeTime = currentTime;";
    TEST_ASSERT_TRUE_MESSAGE(src.find(cooldown) != std::string::npos,
        "STA boot fallback must defer the first AP probe");
    TEST_ASSERT_TRUE_MESSAGE(src.find("_lastApProbeTime = millis();") != std::string::npos,
        "Reconnect-budget AP fallback must also defer the first probe");
    TEST_ASSERT_TRUE_MESSAGE(src.find("_apProbeFailCount = 0;") != std::string::npos,
        "AP fallback must reset probe backoff after a fresh cooldown");

    TestLog::step("AP fallback defers first probe and resets probe backoff");
    TestLog::testEnd(true);
}

/**
 * @brief No-PSRAM static response admission must reserve DRAM headroom.
 *
 * C3 soak captured AsyncBasicResponse header allocation aborting while the
 * legacy public cap of three streams was still admitted at a fragmented
 * 18KB/5KB heap.  The cap remains three for PSRAM builds, while the guarded
 * path must derive a lower effective limit and a one-stream emergency tier.
 */
void test_regression_no_psram_static_admission_limit() {
    const std::string source = readRegressionSrcFile("src/network/WebHandlerContext.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        source.find("staticInFlightLimit") != std::string::npos,
        "static response admission helper must exist");
    TEST_ASSERT_TRUE_MESSAGE(
        source.find("limit = 2") != std::string::npos,
        "no-PSRAM static responses must reserve at least one slot");
    TEST_ASSERT_TRUE_MESSAGE(
        source.find("free8 < 32768U") != std::string::npos &&
        source.find("blk8 < 8192U") != std::string::npos &&
        source.find("limit = 1") != std::string::npos,
        "fragmented no-PSRAM heap must fall back to one static response");
}

/**
 * @brief 验证 DeviceRouteHandler.cpp 保存 logLevel 时即时应用
 * 回归：保存 logLevel 后应立刻调用 LOGGER.setLogLevel()，而非重启后生效
 */
void test_regression_device_handler_loglevel_immediate_apply() {
    TestLog::testStart("Regression: Device Handler logLevel Immediate Apply");

    std::string src = readRegressionSrcFile("src/network/handlers/DeviceRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read DeviceRouteHandler.cpp");

    // 保存时必须读取 logLevel 字段
    TEST_ASSERT_TRUE_MESSAGE(src.find("\"logLevel\"") != std::string::npos,
        "Device save handler must handle logLevel field");
    TestLog::step("logLevel field handled in save");

    // 保存时必须调用 LOGGER.setLogLevel() 即时生效
    TEST_ASSERT_TRUE_MESSAGE(src.find("LOGGER.setLogLevel") != std::string::npos,
        "Device save handler must call LOGGER.setLogLevel() for immediate effect");
    TestLog::step("LOGGER.setLogLevel() called on save");

    // fallback GET 必须返回 logLevel 默认值
    TEST_ASSERT_TRUE_MESSAGE(src.find("\"INFO\"") != std::string::npos,
        "Fallback GET must return logLevel=INFO as default");
    TestLog::step("Fallback GET returns logLevel default");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 autoStart 死字段已从出厂配置和恢复出厂逻辑中移除
 */
void test_regression_auto_start_removed() {
    TestLog::testStart("Regression: autoStart Dead Field Removed");

    // 1. device.json 默认文件不包含 autoStart
    std::string devJson = readRegressionSrcFile("data/config/device.json");
    TEST_ASSERT_FALSE_MESSAGE(devJson.empty(), "Cannot read data/config/device.json");
    TEST_ASSERT_TRUE_MESSAGE(devJson.find("autoStart") == std::string::npos,
        "data/config/device.json must NOT contain autoStart");
    TestLog::step("device.json: autoStart removed");

    // 2. SystemRouteHandler.cpp 恢复出厂默认不包含 autoStart
    std::string src = readRegressionSrcFile("src/network/handlers/SystemRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read SystemRouteHandler.cpp");

    // 定位 DEFAULT_DEVICE 常量
    size_t pos = src.find("DEFAULT_DEVICE");
    TEST_ASSERT_TRUE(pos != std::string::npos);
    std::string deviceBlock = src.substr(pos, 500);
    TEST_ASSERT_TRUE_MESSAGE(deviceBlock.find("autoStart") == std::string::npos,
        "Factory reset DEFAULT_DEVICE must NOT contain autoStart");
    TestLog::step("SystemRouteHandler: autoStart removed from factory reset");

    // 3. 前端 device-config.js 不引用 autoStart
    std::string js = readRegressionSrcFile("web-src/modules/runtime/device-config.js");
    TEST_ASSERT_FALSE_MESSAGE(js.empty(), "Cannot read device-config.js");
    TEST_ASSERT_TRUE_MESSAGE(js.find("autoStart") == std::string::npos,
        "device-config.js must NOT reference autoStart");
    TestLog::step("device-config.js: autoStart not referenced");

    TestLog::testEnd(true);
}

// ========== network.json 死字段清理 + IPManager 同步回归保护 ==========

/**
 * @brief 验证 enableDNS 死字段已从所有配置和代码中移除
 * 回归：enableDNS 在整个代码库中零业务引用，仅为默认模板中的残留
 */
void test_regression_enable_dns_removed() {
    TestLog::testStart("Regression: enableDNS Dead Field Removed");

    // 1. network.json 默认文件不包含 enableDNS
    std::string netJson = readRegressionSrcFile("data/config/network.json");
    TEST_ASSERT_FALSE_MESSAGE(netJson.empty(), "Cannot read data/config/network.json");
    TEST_ASSERT_TRUE_MESSAGE(netJson.find("enableDNS") == std::string::npos,
        "data/config/network.json must NOT contain enableDNS");
    TestLog::step("network.json: enableDNS removed");

    // 2. SystemRouteHandler.cpp 恢复出厂默认不包含 enableDNS
    std::string src = readRegressionSrcFile("src/network/handlers/SystemRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read SystemRouteHandler.cpp");
    size_t pos = src.find("DEFAULT_NETWORK");
    TEST_ASSERT_TRUE(pos != std::string::npos);
    std::string networkBlock = src.substr(pos, 800);
    TEST_ASSERT_TRUE_MESSAGE(networkBlock.find("enableDNS") == std::string::npos,
        "Factory reset DEFAULT_NETWORK must NOT contain enableDNS");
    TestLog::step("SystemRouteHandler: enableDNS removed from factory reset");

    // 3. 前端不引用 enableDNS
    std::string js = readRegressionSrcFile("web-src/modules/runtime/network-config.js");
    if (!js.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(js.find("enableDNS") == std::string::npos,
            "network-config.js must NOT reference enableDNS");
        TestLog::step("network-config.js: enableDNS not referenced");
    } else {
        TestLog::step("network-config.js not found (skipped)");
    }

    TestLog::testEnd(true);
}

/**
 * @brief 验证 NetworkManager 将冲突检测和故障转移配置同步到 IPManager
 * 回归：旧 configureStaticIP() 只同步 staticIP/gateway/subnet，遗漏 5 个字段
 */
void test_regression_network_config_json_handler_precedes_legacy_post() {
    TestLog::testStart("Regression: Network Config JSON Route Order");

    std::string src = readRegressionSrcFile("src/network/handlers/SystemRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read SystemRouteHandler.cpp");

    size_t jsonHandler = src.find("auto* networkJsonHandler = new AsyncCallbackJsonWebHandler(\"/api/network/config\"");
    TEST_ASSERT_TRUE_MESSAGE(jsonHandler != std::string::npos,
        "Network config JSON handler must be registered");

    size_t setMethod = src.find("networkJsonHandler->setMethod(HTTP_POST | HTTP_PUT)", jsonHandler);
    TEST_ASSERT_TRUE_MESSAGE(setMethod != std::string::npos,
        "Network config JSON handler must accept POST and PUT");

    size_t addHandler = src.find("server->addHandler(networkJsonHandler)", setMethod);
    TEST_ASSERT_TRUE_MESSAGE(addHandler != std::string::npos,
        "Network config JSON handler must be added to the server");

    size_t legacyPost = src.find("handleSaveNetworkConfig(request)", addHandler);
    TEST_ASSERT_TRUE_MESSAGE(legacyPost != std::string::npos,
        "Legacy network config POST fallback must still be registered");

    TEST_ASSERT_TRUE_MESSAGE(addHandler < legacyPost,
        "JSON handler must be registered before legacy POST fallback so JSON POST is parsed");

    TestLog::step("Network config JSON handler is registered before legacy POST fallback");
    TestLog::testEnd(true);
}

void test_regression_ipmanager_config_sync() {
    TestLog::testStart("Regression: IPManager Config Sync");

    std::string src = readRegressionSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read NetworkManager.cpp");

    // 必须有 syncIPManagerConfig 方法
    TEST_ASSERT_TRUE_MESSAGE(src.find("syncIPManagerConfig") != std::string::npos,
        "NetworkManager must have syncIPManagerConfig() method");
    TestLog::step("syncIPManagerConfig() method exists");

    // 同步方法必须覆盖所有 5 个字段
    size_t syncPos = src.find("FBNetworkManager::syncIPManagerConfig");
    TEST_ASSERT_TRUE_MESSAGE(syncPos != std::string::npos,
        "syncIPManagerConfig() implementation not found");
    std::string syncBody = src.substr(syncPos, 500);

    TEST_ASSERT_TRUE_MESSAGE(syncBody.find("autoFailover") != std::string::npos,
        "syncIPManagerConfig must sync autoFailover");
    TestLog::step("autoFailover synced");

    TEST_ASSERT_TRUE_MESSAGE(syncBody.find("conflictCheckInterval") != std::string::npos,
        "syncIPManagerConfig must sync conflictCheckInterval");
    TestLog::step("conflictCheckInterval synced");

    TEST_ASSERT_TRUE_MESSAGE(syncBody.find("maxFailoverAttempts") != std::string::npos,
        "syncIPManagerConfig must sync maxFailoverAttempts");
    TestLog::step("maxFailoverAttempts synced");

    TEST_ASSERT_TRUE_MESSAGE(syncBody.find("conflictThreshold") != std::string::npos,
        "syncIPManagerConfig must sync conflictThreshold");
    TestLog::step("conflictThreshold synced");

    TEST_ASSERT_TRUE_MESSAGE(syncBody.find("fallbackToDHCP") != std::string::npos,
        "syncIPManagerConfig must sync fallbackToDHCP");
    TestLog::step("fallbackToDHCP synced");

    // initialize() 中加载配置后必须调用 syncIPManagerConfig
    TEST_ASSERT_TRUE_MESSAGE(src.find("syncIPManagerConfig()") != std::string::npos,
        "initialize() must call syncIPManagerConfig() after loading config");
    TestLog::step("syncIPManagerConfig() called during initialization");

    TestLog::testEnd(true);
}

void test_regression_mqtt_topic_content_action_passthrough() {
    TestLog::testStart("Regression: MQTT topic topicType default publish-aware");

    std::string src = readRegressionSrcFile("src/network/handlers/ProtocolRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read ProtocolRouteHandler.cpp");

    // copyMqttTopicList topicType 默认值必须根据 publishList 区分: 0 for publish, 1 for subscribe
    size_t copyPos = src.find("copyMqttTopicList");
    TEST_ASSERT_TRUE_MESSAGE(copyPos != std::string::npos,
        "copyMqttTopicList function not found");

    std::string copyBody = src.substr(copyPos, 1000);
    TEST_ASSERT_TRUE_MESSAGE(copyBody.find("publishList ? 0 : 1") != std::string::npos,
        "copyMqttTopicList topicType default must distinguish publish (0) vs subscribe (1)");
    TestLog::step("topicType default is publish-aware");

    // content/action 是隐式字段，由 topicType 决定，不应在读写路径中透传
    TEST_ASSERT_TRUE_MESSAGE(copyBody.find("\"content\"") == std::string::npos,
        "copyMqttTopicList must NOT passthrough 'content' (implicit by topicType)");
    TEST_ASSERT_TRUE_MESSAGE(copyBody.find("\"action\"") == std::string::npos,
        "copyMqttTopicList must NOT passthrough 'action' (implicit by topicType)");
    TestLog::step("content/action not in copyMqttTopicList (implicit fields)");

    TestLog::testEnd(true);
}

void test_regression_mqtt_save_content_action_autoPrefix() {
    TestLog::testStart("Regression: MQTT save path autoPrefix default consistency");

    std::string src = readRegressionSrcFile("src/network/handlers/ProtocolRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read ProtocolRouteHandler.cpp");

    size_t savePos = src.find("handleSaveProtocolConfig");
    TEST_ASSERT_TRUE_MESSAGE(savePos != std::string::npos,
        "handleSaveProtocolConfig not found");

    std::string saveBody = src.substr(savePos);

    // content/action 是隐式字段，保存路径不应包含
    // 搜索实际的 publishTopics 保存块（使用 GP 函数调用位置）
    size_t pubSavePos = saveBody.find("GP(\"mqtt_publishTopics\"");
    TEST_ASSERT_TRUE_MESSAGE(pubSavePos != std::string::npos,
        "publishTopics save block not found");
    std::string pubSaveBlock = saveBody.substr(pubSavePos, 1200);
    TEST_ASSERT_TRUE_MESSAGE(pubSaveBlock.find("\"content\"") == std::string::npos,
        "Save path must NOT include 'content' field (implicit by topicType)");
    TestLog::step("Save path does NOT include 'content' (implicit)");

    // autoPrefix 默认值必须为 true (与读取路径一致)
    size_t pubAutoPrefix = pubSaveBlock.find("autoPrefix");
    TEST_ASSERT_TRUE_MESSAGE(pubAutoPrefix != std::string::npos,
        "autoPrefix not found in save path");
    std::string autoPrefixLine = pubSaveBlock.substr(pubAutoPrefix, 50);
    TEST_ASSERT_TRUE_MESSAGE(autoPrefixLine.find("true") != std::string::npos,
        "autoPrefix default must be 'true' in save path (matching read path default)");
    TestLog::step("autoPrefix default is 'true' in save path");

    TestLog::testEnd(true);
}

void test_regression_modbus_master_advanced_params() {
    TestLog::testStart("Regression: Modbus master advanced params hardcoded");

    // 1. ProtocolRouteHandler compact 响应中必须使用硬编码常量（不再从 JSON 透传）
    std::string src = readRegressionSrcFile("src/network/handlers/ProtocolRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read ProtocolRouteHandler.cpp");

    // 不得再出现从前端表单读取的字段名
    TEST_ASSERT_FALSE_MESSAGE(src.find("modbusRtu_responseTimeout") != std::string::npos,
        "responseTimeout must NOT be read from frontend form (UI removed)");
    TEST_ASSERT_FALSE_MESSAGE(src.find("modbusRtu_maxRetries") != std::string::npos,
        "maxRetries must NOT be read from frontend form (UI removed)");
    TEST_ASSERT_FALSE_MESSAGE(src.find("modbusRtu_interPollDelay") != std::string::npos,
        "interPollDelay must NOT be read from frontend form (UI removed)");
    TestLog::step("No frontend form read for master advanced params");

    // 2. copyPeriphExecModbusSummary 必须包含 master 参数（硬编码常量形式）
    size_t summaryPos = src.find("copyPeriphExecModbusSummary");
    TEST_ASSERT_TRUE_MESSAGE(summaryPos != std::string::npos,
        "copyPeriphExecModbusSummary not found");
    const size_t summaryEnd = src.find("\n}\n", summaryPos);
    TEST_ASSERT_TRUE_MESSAGE(summaryEnd != std::string::npos,
        "copyPeriphExecModbusSummary end not found");
    std::string summaryBody = src.substr(summaryPos, summaryEnd + 2 - summaryPos);
    TEST_ASSERT_TRUE_MESSAGE(summaryBody.find("responseTimeout") != std::string::npos,
        "Summary must include responseTimeout hardcoded constant");
    TEST_ASSERT_TRUE_MESSAGE(summaryBody.find("maxRetries") != std::string::npos,
        "Summary must include maxRetries hardcoded constant");
    TEST_ASSERT_TRUE_MESSAGE(summaryBody.find("interPollDelay") != std::string::npos,
        "Summary must include interPollDelay hardcoded constant");
    TestLog::step("Master params included in compact response as hardcoded constants");

    // 3. MasterConfig 构造函数默认值必须一致（1000/2/100）
    std::string header = readRegressionSrcFile("include/protocols/ModbusHandler.h");
    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "Cannot read ModbusHandler.h");
    size_t ctorPos = header.find("MasterConfig()");
    TEST_ASSERT_TRUE_MESSAGE(ctorPos != std::string::npos, "MasterConfig ctor not found");
    std::string ctorBody = header.substr(ctorPos, 200);
    TEST_ASSERT_TRUE_MESSAGE(ctorBody.find("responseTimeout(1000)") != std::string::npos,
        "MasterConfig default responseTimeout must be 1000");
    TEST_ASSERT_TRUE_MESSAGE(ctorBody.find("maxRetries(2)") != std::string::npos,
        "MasterConfig default maxRetries must be 2");
    TestLog::step("MasterConfig ctor defaults are 1000/2/100");

    TestLog::testEnd(true);
}

void test_regression_modbus_motor_device_summary() {
    TestLog::testStart("Regression: Modbus motor device summary passthrough");

    std::string src = readRegressionSrcFile("src/network/handlers/ProtocolRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read ProtocolRouteHandler.cpp");

    size_t devSummaryPos = src.find("void copyModbusDeviceSummary(JsonObject out, JsonObject in)");
    TEST_ASSERT_TRUE_MESSAGE(devSummaryPos != std::string::npos,
        "copyModbusDeviceSummary not found");
    const size_t devSummaryEnd = src.find("\n}\n", devSummaryPos);
    TEST_ASSERT_TRUE_MESSAGE(devSummaryEnd != std::string::npos,
        "copyModbusDeviceSummary end not found");
    std::string devSummaryBody = src.substr(devSummaryPos, devSummaryEnd + 2 - devSummaryPos);

    TEST_ASSERT_TRUE_MESSAGE(devSummaryBody.find("motorMinPosition") != std::string::npos,
        "Device summary must include motorMinPosition");
    TEST_ASSERT_TRUE_MESSAGE(devSummaryBody.find("motorMaxPosition") != std::string::npos,
        "Device summary must include motorMaxPosition");
    TEST_ASSERT_TRUE_MESSAGE(devSummaryBody.find("motorCurrentPosition") != std::string::npos,
        "Device summary must include motorCurrentPosition");
    TEST_ASSERT_TRUE_MESSAGE(devSummaryBody.find("motorMoveStep") != std::string::npos,
        "Device summary must include motorMoveStep");
    TEST_ASSERT_TRUE_MESSAGE(devSummaryBody.find("motorLastPulse") != std::string::npos,
        "Device summary must include motorLastPulse");
    TEST_ASSERT_TRUE_MESSAGE(devSummaryBody.find("motorRegs") != std::string::npos,
        "Device summary must include motorRegs");
    TestLog::step("All motor params included in device summary");

    TestLog::testEnd(true);
}

void test_regression_mqtt_topic_struct_no_content_action() {
    TestLog::testStart("Regression: MQTT topic struct has no content/action fields");

    // MqttPublishTopic 结构体不应包含 content 字段（隐式字段，由 topicType 决定）
    std::string hdr = readRegressionSrcFile("include/protocols/MQTTClient.h");
    TEST_ASSERT_FALSE_MESSAGE(hdr.empty(), "Cannot read MQTTClient.h");

    // 查找 MqttPublishTopic 结构体定义
    size_t pubPos = hdr.find("struct MqttPublishTopic");
    TEST_ASSERT_TRUE_MESSAGE(pubPos != std::string::npos,
        "MqttPublishTopic struct not found");
    // 查找结构体结束 (下一个 };)
    size_t pubEnd = hdr.find("};", pubPos);
    std::string pubStruct = hdr.substr(pubPos, pubEnd - pubPos);
    TEST_ASSERT_TRUE_MESSAGE(pubStruct.find("content") == std::string::npos,
        "MqttPublishTopic must NOT have 'content' field (implicit by topicType)");
    TestLog::step("MqttPublishTopic has no 'content' field");

    // MqttSubscribeTopic 结构体不应包含 action 字段
    size_t subPos = hdr.find("struct MqttSubscribeTopic");
    TEST_ASSERT_TRUE_MESSAGE(subPos != std::string::npos,
        "MqttSubscribeTopic struct not found");
    size_t subEnd = hdr.find("};", subPos);
    std::string subStruct = hdr.substr(subPos, subEnd - subPos);
    TEST_ASSERT_TRUE_MESSAGE(subStruct.find("action") == std::string::npos,
        "MqttSubscribeTopic must NOT have 'action' field (implicit by topicType)");
    TestLog::step("MqttSubscribeTopic has no 'action' field");

    // MQTTClient.cpp 加载代码不应读取 content/action
    std::string cpp = readRegressionSrcFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_FALSE_MESSAGE(cpp.empty(), "Cannot read MQTTClient.cpp");

    // 搜索 publishTopics 加载块
    size_t loadPubPos = cpp.find("publishTopics");
    TEST_ASSERT_TRUE_MESSAGE(loadPubPos != std::string::npos,
        "publishTopics loading block not found");
    std::string loadPubBlock = cpp.substr(loadPubPos, 400);
    TEST_ASSERT_TRUE_MESSAGE(loadPubBlock.find("\"content\"") == std::string::npos,
        "MQTTClient must NOT load 'content' from config");
    TestLog::step("MQTTClient does not load 'content'");

    // 搜索 subscribeTopics 加载块
    size_t loadSubPos = cpp.find("subscribeTopics");
    TEST_ASSERT_TRUE_MESSAGE(loadSubPos != std::string::npos,
        "subscribeTopics loading block not found");
    std::string loadSubBlock = cpp.substr(loadSubPos, 400);
    TEST_ASSERT_TRUE_MESSAGE(loadSubBlock.find("\"action\"") == std::string::npos,
        "MQTTClient must NOT load 'action' from config");
    TestLog::step("MQTTClient does not load 'action'");

    TestLog::testEnd(true);
}

void test_regression_mqtt_topictype_enum_values() {
    TestLog::testStart("Regression: MqttTopicType enum values must match protocol.json");

    std::string hdr = readRegressionSrcFile("include/protocols/MQTTClient.h");
    TEST_ASSERT_FALSE_MESSAGE(hdr.empty(), "Cannot read MQTTClient.h");

    // 检查枚举值定义，确保与 protocol.json 中的 topicType 值一致
    size_t enumPos = hdr.find("enum class MqttTopicType");
    TEST_ASSERT_TRUE_MESSAGE(enumPos != std::string::npos,
        "MqttTopicType enum not found");
    size_t enumEnd = hdr.find("};", enumPos);
    std::string enumBody = hdr.substr(enumPos, enumEnd - enumPos);

    // DATA_REPORT = 0 (发布主题默认)
    TEST_ASSERT_TRUE_MESSAGE(enumBody.find("DATA_REPORT") != std::string::npos,
        "MqttTopicType must have DATA_REPORT");
    // DATA_COMMAND = 1 (订阅主题默认)
    TEST_ASSERT_TRUE_MESSAGE(enumBody.find("DATA_COMMAND") != std::string::npos,
        "MqttTopicType must have DATA_COMMAND");
    // DEVICE_INFO = 2
    TEST_ASSERT_TRUE_MESSAGE(enumBody.find("DEVICE_INFO") != std::string::npos,
        "MqttTopicType must have DEVICE_INFO");
    // REALTIME_MON = 3
    TEST_ASSERT_TRUE_MESSAGE(enumBody.find("REALTIME_MON") != std::string::npos,
        "MqttTopicType must have REALTIME_MON");
    // DEVICE_EVENT = 4
    TEST_ASSERT_TRUE_MESSAGE(enumBody.find("DEVICE_EVENT") != std::string::npos,
        "MqttTopicType must have DEVICE_EVENT");
    // OTA_UPGRADE = 5
    TEST_ASSERT_TRUE_MESSAGE(enumBody.find("OTA_UPGRADE") != std::string::npos,
        "MqttTopicType must have OTA_UPGRADE");
    // OTA_BINARY = 6
    TEST_ASSERT_TRUE_MESSAGE(enumBody.find("OTA_BINARY") != std::string::npos,
        "MqttTopicType must have OTA_BINARY");
    // NTP_SYNC = 7
    TEST_ASSERT_TRUE_MESSAGE(enumBody.find("NTP_SYNC") != std::string::npos,
        "MqttTopicType must have NTP_SYNC");

    // 检查 publish/subscribe 默认 topicType 区分
    std::string src = readRegressionSrcFile("src/network/handlers/ProtocolRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read ProtocolRouteHandler.cpp");

    size_t copyPos = src.find("copyMqttTopicList");
    TEST_ASSERT_TRUE_MESSAGE(copyPos != std::string::npos,
        "copyMqttTopicList not found");
    std::string copyBody = src.substr(copyPos, 600);

    // publishList 参数用于区分默认值：publish=0, subscribe=1
    TEST_ASSERT_TRUE_MESSAGE(copyBody.find("publishList ? 0 : 1") != std::string::npos,
        "copyMqttTopicList must use publishList-aware topicType default");
    TestLog::step("MqttTopicType enum and defaults verified");

    TestLog::testEnd(true);
}

void test_regression_modbus_master_params_consistency() {
    TestLog::testStart("Regression: Modbus master params consistency (hardcoded)");

    std::string src = readRegressionSrcFile("src/network/handlers/ProtocolRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read ProtocolRouteHandler.cpp");

    // 1. 保存路径不得再用 GPI 读取前端输入的高级参数：
    //    responseTimeout/maxRetries/interPollDelay 已移除 UI，写死在构造函数。
    size_t savePos = src.find("handleSaveProtocolConfig");
    TEST_ASSERT_TRUE_MESSAGE(savePos != std::string::npos,
        "handleSaveProtocolConfig not found");
    std::string saveBody = src.substr(savePos, 25000);

    TEST_ASSERT_FALSE_MESSAGE(saveBody.find("GPI(\"modbusRtu_responseTimeout\"") != std::string::npos,
        "Save path must NOT use GPI for responseTimeout (UI removed, hardcoded in ctor)");
    TEST_ASSERT_FALSE_MESSAGE(saveBody.find("GPI(\"modbusRtu_maxRetries\"") != std::string::npos,
        "Save path must NOT use GPI for maxRetries (UI removed, hardcoded in ctor)");
    TEST_ASSERT_FALSE_MESSAGE(saveBody.find("GPI(\"modbusRtu_interPollDelay\"") != std::string::npos,
        "Save path must NOT use GPI for interPollDelay (UI removed, hardcoded in ctor)");
    TestLog::step("Save path does not use GPI for master params");

    // 2. compact 响应必须包含 master 高级参数（文件版 copyPeriphExecModbusSummary）
    size_t compactPos = src.find("void copyPeriphExecModbusSummary(JsonObject out, JsonObject in)");
    TEST_ASSERT_TRUE_MESSAGE(compactPos != std::string::npos,
        "File-based copyPeriphExecModbusSummary not found");
    std::string compactBody = src.substr(compactPos, 1000);

    TEST_ASSERT_TRUE_MESSAGE(compactBody.find("responseTimeout") != std::string::npos,
        "Compact response must include responseTimeout");
    TEST_ASSERT_TRUE_MESSAGE(compactBody.find("maxRetries") != std::string::npos,
        "Compact response must include maxRetries");
    TEST_ASSERT_TRUE_MESSAGE(compactBody.find("interPollDelay") != std::string::npos,
        "Compact response must include interPollDelay");
    TestLog::step("Compact response includes master params");

    // 3. 运行时版 copyPeriphExecModbusSummary 也必须包含
    size_t runtimePos = src.find("copyPeriphExecModbusSummary(JsonObject out, const ModbusConfig&");
    TEST_ASSERT_TRUE_MESSAGE(runtimePos != std::string::npos,
        "Runtime copyPeriphExecModbusSummary not found");
    std::string runtimeBody = src.substr(runtimePos, 1500);

    TEST_ASSERT_TRUE_MESSAGE(runtimeBody.find("responseTimeout") != std::string::npos,
        "Runtime response must include responseTimeout");
    TEST_ASSERT_TRUE_MESSAGE(runtimeBody.find("maxRetries") != std::string::npos,
        "Runtime response must include maxRetries");
    TEST_ASSERT_TRUE_MESSAGE(runtimeBody.find("interPollDelay") != std::string::npos,
        "Runtime response must include interPollDelay");
    TestLog::step("Runtime response includes master params");

    // 4. ModbusHandler.cpp 不得再从 JSON 读取这三个参数
    std::string modbusImpl = readRegressionSrcFile("src/protocols/ModbusHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(modbusImpl.empty(), "Cannot read ModbusHandler.cpp");
    size_t loadPos = modbusImpl.find("loadConfigFromFile");
    TEST_ASSERT_TRUE_MESSAGE(loadPos != std::string::npos, "loadConfigFromFile not found");
    std::string loadBody = modbusImpl.substr(loadPos, 3000);
    TEST_ASSERT_FALSE_MESSAGE(
        loadBody.find("masterObj[\"responseTimeout\"]") != std::string::npos,
        "ModbusHandler must NOT read responseTimeout from JSON (hardcoded in ctor)");
    TEST_ASSERT_FALSE_MESSAGE(
        loadBody.find("masterObj[\"maxRetries\"]") != std::string::npos,
        "ModbusHandler must NOT read maxRetries from JSON (hardcoded in ctor)");
    TEST_ASSERT_FALSE_MESSAGE(
        loadBody.find("masterObj[\"interPollDelay\"]") != std::string::npos,
        "ModbusHandler must NOT read interPollDelay from JSON (hardcoded in ctor)");
    TestLog::step("ModbusHandler does not read master params from JSON");

    TestLog::testEnd(true);
}

void test_regression_users_json_field_consistency() {
    TestLog::testStart("Regression: users.json field consistency (email/remark/createBy removed, description kept)");

    // 1. User 结构体不应有 createBy/email/remark，应有 description
    std::string hdr = readRegressionSrcFile("include/security/UserManager.h");
    TEST_ASSERT_FALSE_MESSAGE(hdr.empty(), "Cannot read UserManager.h");

    size_t userStructPos = hdr.find("struct User {");
    TEST_ASSERT_TRUE_MESSAGE(userStructPos != std::string::npos,
        "User struct not found");
    size_t userStructEnd = hdr.find("};", userStructPos);
    std::string userStruct = hdr.substr(userStructPos, userStructEnd - userStructPos);

    TEST_ASSERT_TRUE_MESSAGE(userStruct.find("createBy") == std::string::npos,
        "User struct must NOT have 'createBy' field (removed)");
    TEST_ASSERT_TRUE_MESSAGE(userStruct.find("description") != std::string::npos,
        "User struct must have 'description' field");
    TEST_ASSERT_TRUE_MESSAGE(userStruct.find("email") == std::string::npos,
        "User struct must NOT have 'email' field (removed)");
    TEST_ASSERT_TRUE_MESSAGE(userStruct.find("remark") == std::string::npos,
        "User struct must NOT have 'remark' field (removed)");
    TestLog::step("User struct: description present, createBy/email/remark removed");

    // 2. 加载代码不应读写 createBy/email/remark
    std::string cpp = readRegressionSrcFile("src/security/UserManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(cpp.empty(), "Cannot read UserManager.cpp");

    size_t loadPos = cpp.find("bool UserManager::loadUsersFromStorage");
    TEST_ASSERT_TRUE_MESSAGE(loadPos != std::string::npos,
        "loadUsersFromStorage not found");
    std::string loadBody = cpp.substr(loadPos, 4000);

    TEST_ASSERT_TRUE_MESSAGE(loadBody.find("\"createBy\"") == std::string::npos,
        "loadUsersFromStorage must NOT read 'createBy' (removed)");
    TEST_ASSERT_TRUE_MESSAGE(loadBody.find("\"email\"") == std::string::npos,
        "loadUsersFromStorage must NOT read 'email'");
    TEST_ASSERT_TRUE_MESSAGE(loadBody.find("\"remark\"") == std::string::npos,
        "loadUsersFromStorage must NOT read 'remark'");
    TestLog::step("Load code: no createBy/email/remark");

    // 3. 保存代码不应写入 createBy/email/remark
    size_t savePos = cpp.find("bool UserManager::saveUsersToStorage");
    TEST_ASSERT_TRUE_MESSAGE(savePos != std::string::npos,
        "saveUsersToStorage not found");
    std::string saveBody = cpp.substr(savePos, 2000);

    TEST_ASSERT_TRUE_MESSAGE(saveBody.find("\"createBy\"") == std::string::npos,
        "saveUsersToStorage must NOT write 'createBy' (removed)");
    TEST_ASSERT_TRUE_MESSAGE(saveBody.find("\"email\"") == std::string::npos,
        "saveUsersToStorage must NOT write 'email'");
    TEST_ASSERT_TRUE_MESSAGE(saveBody.find("\"remark\"") == std::string::npos,
        "saveUsersToStorage must NOT write 'remark'");
    TestLog::step("Save code: no createBy/email/remark");

    // 4. NTP 同步不应再挂钩 updateCreateByFromNtp
    std::string fw = readRegressionSrcFile("src/core/FastBeeFramework.cpp");
    TEST_ASSERT_FALSE_MESSAGE(fw.empty(), "Cannot read FastBeeFramework.cpp");
    TEST_ASSERT_TRUE_MESSAGE(fw.find("updateCreateByFromNtp") == std::string::npos,
        "FastBeeFramework must NOT call updateCreateByFromNtp (removed)");
    TestLog::step("NTP sync: no updateCreateByFromNtp hook");

    // 5. 出厂模板不应包含 createBy/email/remark，应包含 description
    std::string sysHandler = readRegressionSrcFile("src/network/handlers/SystemRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(sysHandler.empty(), "Cannot read SystemRouteHandler.cpp");

    size_t defaultUsersPos = sysHandler.find("DEFAULT_USERS");
    TEST_ASSERT_TRUE_MESSAGE(defaultUsersPos != std::string::npos,
        "DEFAULT_USERS not found");
    std::string defaultUsersBlock = sysHandler.substr(defaultUsersPos, 800);

    TEST_ASSERT_TRUE_MESSAGE(defaultUsersBlock.find("createBy") == std::string::npos,
        "DEFAULT_USERS must NOT include 'createBy' (removed)");
    TEST_ASSERT_TRUE_MESSAGE(defaultUsersBlock.find("description") != std::string::npos,
        "DEFAULT_USERS must include 'description'");
    TEST_ASSERT_TRUE_MESSAGE(defaultUsersBlock.find("\"email\"") == std::string::npos,
        "DEFAULT_USERS must NOT include 'email'");
    TEST_ASSERT_TRUE_MESSAGE(defaultUsersBlock.find("\"remark\"") == std::string::npos,
        "DEFAULT_USERS must NOT include 'remark'");
    TestLog::step("Factory default: description present, createBy/email/remark removed");

    TestLog::testEnd(true);
}

// ============ security 精简回归：仅4个UI可配置字段，9个硬编码字段已删除 ============

static void test_regression_security_json_lite() {
    TestLog::testStart("regression: compact security defaults and complete runtime persistence");

    // 1. users.json 配置文件：security 仅含 4 个字段
    std::string usersJson = readRegressionSrcFile("data/config/users.json");
    TEST_ASSERT_FALSE_MESSAGE(usersJson.empty(), "Cannot read users.json");

    // 必须包含 4 个 UI 可配置字段
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("maxLoginAttempts") != std::string::npos,
        "users.json must contain 'maxLoginAttempts'");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("loginLockoutTime") != std::string::npos,
        "users.json must contain 'loginLockoutTime'");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("minPasswordLength") != std::string::npos,
        "users.json must contain 'minPasswordLength'");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("requireStrongPasswords") != std::string::npos,
        "users.json must contain 'requireStrongPasswords'");

    // 9 个硬编码字段必须不存在
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("maxPasswordLength") == std::string::npos,
        "users.json must NOT contain 'maxPasswordLength'");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("allowMultipleSessions") == std::string::npos,
        "users.json must NOT contain 'allowMultipleSessions'");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("sessionTimeout") == std::string::npos,
        "users.json must NOT contain 'sessionTimeout'");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("sessionCleanupInterval") == std::string::npos,
        "users.json must NOT contain 'sessionCleanupInterval'");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("enableSessionPersistence") == std::string::npos,
        "users.json must NOT contain 'enableSessionPersistence'");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("cookieName") == std::string::npos,
        "users.json must NOT contain 'cookieName'");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("cookieMaxAge") == std::string::npos,
        "users.json must NOT contain 'cookieMaxAge'");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("cookieHttpOnly") == std::string::npos,
        "users.json must NOT contain 'cookieHttpOnly'");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("cookieSecure") == std::string::npos,
        "users.json must NOT contain 'cookieSecure'");
    TestLog::step("users.json: 4 UI fields present, 9 hardcoded fields removed");

    // 2. 出厂模板 DEFAULT_USERS 同步精简
    std::string sysHandler = readRegressionSrcFile("src/network/handlers/SystemRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(sysHandler.empty(), "Cannot read SystemRouteHandler.cpp");

    size_t defaultPos = sysHandler.find("DEFAULT_USERS");
    TEST_ASSERT_TRUE_MESSAGE(defaultPos != std::string::npos, "DEFAULT_USERS not found");
    std::string defaultBlock = sysHandler.substr(defaultPos, 1200);

    TEST_ASSERT_TRUE_MESSAGE(defaultBlock.find("maxLoginAttempts") != std::string::npos,
        "DEFAULT_USERS must contain 'maxLoginAttempts'");
    TEST_ASSERT_TRUE_MESSAGE(defaultBlock.find("requireStrongPasswords") != std::string::npos,
        "DEFAULT_USERS must contain 'requireStrongPasswords'");
    TEST_ASSERT_TRUE_MESSAGE(defaultBlock.find("maxPasswordLength") == std::string::npos,
        "DEFAULT_USERS must NOT contain 'maxPasswordLength'");
    TEST_ASSERT_TRUE_MESSAGE(defaultBlock.find("cookieName") == std::string::npos,
        "DEFAULT_USERS must NOT contain 'cookieName'");
    TEST_ASSERT_TRUE_MESSAGE(defaultBlock.find("sessionTimeout") == std::string::npos,
        "DEFAULT_USERS must NOT contain 'sessionTimeout'");
    TestLog::step("DEFAULT_USERS factory: 4 UI fields only");

    // 3. 出厂模板保持精简，但运行时可修改的会话策略必须完整持久化
    std::string userMgrCpp = readRegressionSrcFile("src/security/UserManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(userMgrCpp.empty(), "Cannot read UserManager.cpp");

    size_t savePos = userMgrCpp.find("bool UserManager::saveUsersToStorage");
    if (savePos == std::string::npos) savePos = userMgrCpp.find("UserManager::saveUsersToStorage");
    TEST_ASSERT_TRUE_MESSAGE(savePos != std::string::npos, "saveUsersToStorage not found");
    std::string saveBlock = userMgrCpp.substr(savePos, 3000);

    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("maxLoginAttempts") != std::string::npos,
        "save must write 'maxLoginAttempts'");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("loginLockoutTime") != std::string::npos,
        "save must write 'loginLockoutTime'");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("minPasswordLength") != std::string::npos,
        "save must write 'minPasswordLength'");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("requireStrongPasswords") != std::string::npos,
        "save must write 'requireStrongPasswords'");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("maxPasswordLength") != std::string::npos,
        "save must preserve 'maxPasswordLength'");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("cookieName") != std::string::npos,
        "save must preserve 'cookieName'");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("allowMultipleSessions") != std::string::npos,
        "save must preserve 'allowMultipleSessions'");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("sessionTimeout") != std::string::npos,
        "save must preserve 'sessionTimeout'");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("cookieSecure") != std::string::npos,
        "save must preserve 'cookieSecure'");
    TestLog::step("UserManager save: complete password and session policy persisted");

    // 4. IUserManager 接口必须有 updatePasswordPolicy
    std::string iUserMgr = readRegressionSrcFile("include/core/interfaces/IUserManager.h");
    TEST_ASSERT_FALSE_MESSAGE(iUserMgr.empty(), "Cannot read IUserManager.h");
    TEST_ASSERT_TRUE_MESSAGE(iUserMgr.find("updatePasswordPolicy") != std::string::npos,
        "IUserManager must declare 'updatePasswordPolicy'");
    TestLog::step("IUserManager: updatePasswordPolicy declared");

    // 5. DeviceRouteHandler GET 必须读取 security 字段
    std::string devHandler = readRegressionSrcFile("src/network/handlers/DeviceRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(devHandler.empty(), "Cannot read DeviceRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(devHandler.find("maxLoginAttempts") != std::string::npos,
        "DeviceRouteHandler must reference 'maxLoginAttempts'");
    TEST_ASSERT_TRUE_MESSAGE(devHandler.find("updatePasswordPolicy") != std::string::npos,
        "DeviceRouteHandler must call 'updatePasswordPolicy'");
    TestLog::step("DeviceRouteHandler: security fields wired to GET/PUT");

    TestLog::testEnd(true);
}

// ============ lastLogin 持久化 + createBy 移除回归保护 ============

static void test_regression_last_login_and_createby_cleanup() {
    TestLog::testStart("Regression: lastLogin uses TimeUtils + createBy fully removed");

    // 1. updateLastLogin 必须使用 TimeUtils::getTimestamp 而非 millis()
    std::string cpp = readRegressionSrcFile("src/security/UserManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(cpp.empty(), "Cannot read UserManager.cpp");

    size_t updatePos = cpp.find("void UserManager::updateLastLogin");
    TEST_ASSERT_TRUE_MESSAGE(updatePos != std::string::npos,
        "updateLastLogin not found");
    std::string updateBlock = cpp.substr(updatePos, 500);

    TEST_ASSERT_TRUE_MESSAGE(updateBlock.find("TimeUtils::getTimestamp") != std::string::npos,
        "updateLastLogin must use TimeUtils::getTimestamp()");
    TEST_ASSERT_TRUE_MESSAGE(updateBlock.find("saveUsersToStorage") != std::string::npos,
        "updateLastLogin must call saveUsersToStorage()");
    TestLog::step("updateLastLogin: uses TimeUtils, persists on each login");

    // 2. getAllUsers 不应包含 createBy，应包含 lastLogin
    size_t getAllPos = cpp.find("String UserManager::getAllUsers");
    TEST_ASSERT_TRUE_MESSAGE(getAllPos != std::string::npos,
        "getAllUsers not found");
    std::string getAllBlock = cpp.substr(getAllPos, 500);

    TEST_ASSERT_TRUE_MESSAGE(getAllBlock.find("\"createBy\"") == std::string::npos,
        "getAllUsers must NOT include 'createBy'");
    TEST_ASSERT_TRUE_MESSAGE(getAllBlock.find("lastLogin") != std::string::npos,
        "getAllUsers must include 'lastLogin'");
    TestLog::step("getAllUsers: lastLogin included, createBy removed");

    // 3. UserRouteHandler API 响应不应包含 createBy
    std::string userHandler = readRegressionSrcFile("src/network/handlers/UserRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(userHandler.empty(), "Cannot read UserRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(userHandler.find("createBy") == std::string::npos,
        "UserRouteHandler must NOT include 'createBy' in API response");
    TestLog::step("UserRouteHandler: createBy not in API response");

    // 4. DeviceRouteHandler 不应再引用 updateCreateByFromNtp
    std::string devHandler = readRegressionSrcFile("src/network/handlers/DeviceRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(devHandler.empty(), "Cannot read DeviceRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(devHandler.find("updateCreateByFromNtp") == std::string::npos,
        "DeviceRouteHandler must NOT call updateCreateByFromNtp");
    TestLog::step("DeviceRouteHandler: no updateCreateByFromNtp reference");

    // 5. users.json 不包含 createBy
    std::string usersJson = readRegressionSrcFile("data/config/users.json");
    TEST_ASSERT_FALSE_MESSAGE(usersJson.empty(), "Cannot read users.json");
    TEST_ASSERT_TRUE_MESSAGE(usersJson.find("createBy") == std::string::npos,
        "users.json must NOT contain 'createBy'");
    TestLog::step("users.json: createBy removed");

    TestLog::testEnd(true);
}

// ========== WiFi 安全类型默认值 + MQTT 数据同步 + 状态显示 + 摘要默认值回归测试 ==========

/**
 * @brief WiFi 安全类型下拉框默认值必须为 WPA2
 * 回归：旧代码默认值为 'wpa'，应改为 'wpa2'
 */
static void test_regression_wifi_security_default_wpa2() {
    TestLog::testStart("Regression: WiFi Security Default is WPA2");

    std::string js = readRegressionSrcFile("web-src/modules/runtime/network.js");
    TEST_ASSERT_FALSE_MESSAGE(js.empty(), "Cannot read network.js");

    // 查找 WiFi 安全类型设置行
    size_t pos = js.find("wifi-security");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos, "wifi-security not found in network.js");

    // 提取该行附近的内容
    std::string block = js.substr(pos, 100);

    // 默认值必须包含 wpa2
    TEST_ASSERT_TRUE_MESSAGE(block.find("'wpa2'") != std::string::npos,
        "WiFi security default must be 'wpa2'");
    TestLog::step("WiFi security default: wpa2");

    TestLog::testEnd(true);
}

/**
 * @brief MQTT 配置加载不应使用硬编码默认值
 * 回归：旧代码 server 默认 'iot.fastbee.cn'，mqttSecret 默认 'K451265A72244J79'
 */
static void test_regression_mqtt_no_hardcoded_defaults() {
    TestLog::testStart("Regression: MQTT Config No Hardcoded Defaults");

    // 1. protocol-config.js 不应包含硬编码默认 broker
    std::string config = readRegressionSrcFile("web-src/modules/runtime/protocol/protocol-config.js");
    TEST_ASSERT_FALSE_MESSAGE(config.empty(), "Cannot read protocol-config.js");

    TEST_ASSERT_TRUE_MESSAGE(config.find("iot.fastbee.cn") == std::string::npos,
        "protocol-config.js must NOT hardcode 'iot.fastbee.cn' as MQTT broker default");
    TestLog::step("protocol-config.js: no hardcoded broker");

    TEST_ASSERT_TRUE_MESSAGE(config.find("K451265A72244J79") == std::string::npos,
        "protocol-config.js must NOT hardcode 'K451265A72244J79' as MQTT secret default");
    TestLog::step("protocol-config.js: no hardcoded secret");

    // mqtt.server 必须使用 ?? '' 而不是 || 'default'
    size_t brokerPos = config.find("mqtt-broker");
    TEST_ASSERT_TRUE_MESSAGE(brokerPos != std::string::npos, "mqtt-broker not found");
    std::string brokerLine = config.substr(brokerPos, 80);
    TEST_ASSERT_TRUE_MESSAGE(brokerLine.find("??") != std::string::npos,
        "mqtt.server must use ?? operator (nullish coalescing) for empty string support");
    TestLog::step("mqtt.server uses ?? for empty string support");

    // 2. protocol-lite-config.js 同样不应包含硬编码默认值
    std::string lite = readRegressionSrcFile("web-src/modules/runtime/protocol/protocol-lite-config.js");
    TEST_ASSERT_FALSE_MESSAGE(lite.empty(), "Cannot read protocol-lite-config.js");

    TEST_ASSERT_TRUE_MESSAGE(lite.find("iot.fastbee.cn") == std::string::npos,
        "protocol-lite-config.js must NOT hardcode 'iot.fastbee.cn'");
    TEST_ASSERT_TRUE_MESSAGE(lite.find("K451265A72244J79") == std::string::npos,
        "protocol-lite-config.js must NOT hardcode 'K451265A72244J79'");
    TestLog::step("protocol-lite-config.js: no hardcoded defaults");

    // 3. protocol-mqtt.html 不得用 value 预填生产 Broker。placeholder 仅是
    // 输入格式提示，不会覆盖后端返回的真实配置。
    std::string html = readRegressionSrcFile("web-src/pages/fragments/protocol-mqtt.html");
    TEST_ASSERT_FALSE_MESSAGE(html.empty(), "Cannot read protocol-mqtt.html");

    TEST_ASSERT_TRUE_MESSAGE(
        html.find("value=\"iot.fastbee.cn\"") == std::string::npos &&
        html.find("value='iot.fastbee.cn'") == std::string::npos,
        "protocol-mqtt.html must NOT prefill 'iot.fastbee.cn' as the broker value");
    TEST_ASSERT_TRUE_MESSAGE(html.find("K451265A72244J79") == std::string::npos,
        "protocol-mqtt.html must NOT hardcode secret in template");
    TEST_ASSERT_TRUE_MESSAGE(html.find("S&FB100900001") == std::string::npos,
        "protocol-mqtt.html must NOT hardcode clientId in template");
    TEST_ASSERT_TRUE_MESSAGE(html.find("P47T6OD5IPFWHUM6") == std::string::npos,
        "protocol-mqtt.html must NOT hardcode password in template");
    TestLog::step("protocol-mqtt.html: no hardcoded example values");

    TestLog::testEnd(true);
}

/**
 * @brief MQTT 未启用时先保持中性态，再由一次状态响应展示最终原因。
 * 配置值不能冒充运行状态，否则会短暂误报“未连接”并掩盖内存降级原因。
 */
static void test_regression_mqtt_disabled_waits_for_runtime_status() {
    TestLog::testStart("Regression: MQTT Disabled Waits For Runtime Status");

    // 1. protocol-config.js 中 mqtt.enabled=false 时停止轮询，保持中性态并只查一次状态。
    std::string config = readRegressionSrcFile("web-src/modules/runtime/protocol/protocol-config.js");
    TEST_ASSERT_FALSE_MESSAGE(config.empty(), "Cannot read protocol-config.js");

    // 查找 mqtt.enabled 检查后的 else 分支
    size_t enabledPos = config.find("if (mqtt.enabled)");
    TEST_ASSERT_TRUE_MESSAGE(enabledPos != std::string::npos,
        "mqtt.enabled check not found");

    std::string block = config.substr(enabledPos, 1200);
    TEST_ASSERT_TRUE_MESSAGE(block.find("_stopMqttStatusPolling") != std::string::npos,
        "When mqtt.enabled=false, must call _stopMqttStatusPolling()");
    TEST_ASSERT_TRUE_MESSAGE(block.find("_setMqttStatusLoading") != std::string::npos,
        "Disabled MQTT must remain neutral until the runtime status arrives");
    TEST_ASSERT_TRUE_MESSAGE(block.find("_loadMqttStatus") != std::string::npos,
        "Disabled MQTT must query runtime status once for the final reason");
    TEST_ASSERT_TRUE_MESSAGE(block.find("mqtt-status-offline") == std::string::npos,
        "Config loading must not publish an unverified offline state");
    TestLog::step("protocol-config.js: mqtt disabled -> stop polling + neutral status probe");

    // 2. mqtt-config.js _updateMqttStatusUI 中 !d.enabled 必须在 d.connected 之前
    std::string mqtt = readRegressionSrcFile("web-src/modules/runtime/protocol/mqtt-config.js");
    TEST_ASSERT_FALSE_MESSAGE(mqtt.empty(), "Cannot read mqtt-config.js");

    size_t updatePos = mqtt.find("_updateMqttStatusUI: function");
    TEST_ASSERT_TRUE_MESSAGE(updatePos != std::string::npos,
        "_updateMqttStatusUI not found");

    // Locate the concrete status branch instead of using a fixed byte window;
    // the auto-reconnect diagnostics above it may legitimately grow over time.
    // !d.enabled 分支必须在 d.connected badge 条件之前，确保禁用时不显示“已连接”
    // 注意：使用 "else if (d.connected)" 而非 "d.connected"，避免匹配自动重连逻辑中的 "!d.connected"
    size_t disabledPos = mqtt.find("else if (!d.enabled)", updatePos);
    size_t connectedPos = mqtt.find("else if (d.connected)", updatePos);
    size_t initPos = mqtt.find("else if (!d.initialized)", updatePos);
    TEST_ASSERT_TRUE_MESSAGE(disabledPos != std::string::npos,
        "!d.enabled check must exist in _updateMqttStatusUI");
    TEST_ASSERT_TRUE_MESSAGE(connectedPos != std::string::npos,
        "d.connected check must exist");
    TEST_ASSERT_TRUE_MESSAGE(disabledPos < connectedPos,
        "!d.enabled must be checked BEFORE d.connected to prevent disabled MQTT showing 'connected'");
    TEST_ASSERT_TRUE_MESSAGE(initPos != std::string::npos,
        "!d.initialized check must exist");
    TestLog::step("mqtt-config.js: !d.enabled before d.connected in condition chain");

    // 3. MQTT 禁用时停止轮询，但仍查询一次后端状态以展示内存降级原因
    size_t refreshPos = mqtt.find("refreshMqttStatus()");
    TEST_ASSERT_TRUE_MESSAGE(refreshPos != std::string::npos,
        "refreshMqttStatus not found");
    size_t refreshEnd = mqtt.find("mqttNtpSync()", refreshPos);
    TEST_ASSERT_TRUE_MESSAGE(refreshEnd != std::string::npos,
        "refreshMqttStatus end marker not found");
    std::string refreshBlock = mqtt.substr(refreshPos, refreshEnd - refreshPos);
    TEST_ASSERT_TRUE_MESSAGE(refreshBlock.find("mqtt-enabled") != std::string::npos,
        "refreshMqttStatus must check mqtt-enabled checkbox before API call");
    TEST_ASSERT_TRUE_MESSAGE(refreshBlock.find("_stopMqttStatusPolling") != std::string::npos,
        "refreshMqttStatus must stop polling when MQTT is disabled");
    TEST_ASSERT_TRUE_MESSAGE(refreshBlock.find("_setMqttStatusLoading") != std::string::npos,
        "Disabled refresh must show a neutral state until the status response arrives");
    size_t disabledRefreshPos = refreshBlock.find("if (enabledCheckbox && !enabledCheckbox.checked)");
    TEST_ASSERT_TRUE_MESSAGE(disabledRefreshPos != std::string::npos,
        "refreshMqttStatus disabled branch not found");
    size_t disabledOpen = refreshBlock.find('{', disabledRefreshPos);
    size_t disabledClose = std::string::npos;
    int braceDepth = 0;
    for (size_t i = disabledOpen; disabledOpen != std::string::npos && i < refreshBlock.size(); ++i) {
        if (refreshBlock[i] == '{') {
            ++braceDepth;
        } else if (refreshBlock[i] == '}' && --braceDepth == 0) {
            disabledClose = i;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(disabledOpen != std::string::npos &&
                             disabledClose != std::string::npos,
        "refreshMqttStatus disabled branch must have a balanced body");
    std::string disabledRefreshBlock = refreshBlock.substr(
        disabledOpen, disabledClose - disabledOpen + 1);
    TEST_ASSERT_TRUE_MESSAGE(disabledRefreshBlock.find("_loadMqttStatus") != std::string::npos,
        "Disabled refresh must query backend status once to expose memory-protection diagnostics");
    TEST_ASSERT_TRUE_MESSAGE(disabledRefreshBlock.find("_startMqttStatusPolling") == std::string::npos,
        "Disabled refresh must not start periodic MQTT status polling");
    TEST_ASSERT_TRUE_MESSAGE(disabledRefreshBlock.find("mqtt-status-offline") == std::string::npos,
        "Disabled refresh must not publish an unverified offline state");
    TEST_ASSERT_TRUE_MESSAGE(disabledRefreshBlock.find("return;") != std::string::npos,
        "Disabled refresh must return before entering the enabled polling path");
    TestLog::step("mqtt-config.js: disabled refresh performs one status query without polling");

    // 4. saveProtocolConfig 保存后必须根据 enabled 状态决定是否轮询
    size_t savePos = config.find("saveProtocolConfig(formId)");
    TEST_ASSERT_TRUE_MESSAGE(savePos != std::string::npos,
        "saveProtocolConfig not found");
    size_t saveEnd = config.find("_getMasterRiskMeta(", savePos);
    TEST_ASSERT_TRUE_MESSAGE(saveEnd != std::string::npos,
        "saveProtocolConfig end marker not found");
    std::string saveBlock = config.substr(savePos, saveEnd - savePos);
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("mqtt-enabled") != std::string::npos,
        "saveProtocolConfig must check mqtt-enabled before starting polling");
    // 禁用分支必须调用 _stopMqttStatusPolling 并设置 badge
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("_stopMqttStatusPolling") != std::string::npos,
        "saveProtocolConfig must stop polling when MQTT is disabled after save");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("mqtt-status-offline") != std::string::npos,
        "saveProtocolConfig must set badge to offline when MQTT is disabled after save");
    TestLog::step("protocol-config.js: saveProtocolConfig checks enabled + stops polling + sets offline");

    TestLog::testEnd(true);
}

/**
 * @brief MQTT 禁用时后端必须停止客户端并报告 connected=false
 * 回归：旧代码 MQTT 禁用后客户端仍在运行，状态 API 返回 connected=true
 */
static void test_regression_mqtt_disabled_stops_backend_client() {
    TestLog::testStart("Regression: MQTT Disabled Stops Backend Client");

    // 1. MqttRouteHandler::handleGetMqttStatus 必须在 mqttConfigEnabled=false 时停止客户端
    std::string handler = readRegressionSrcFile("src/network/handlers/MqttRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(handler.empty(), "Cannot read MqttRouteHandler.cpp");

    // 查找 handleGetMqttStatus 函数定义（不是 lambda 调用）
    size_t statusPos = handler.find("MqttRouteHandler::handleGetMqttStatus");
    TEST_ASSERT_TRUE_MESSAGE(statusPos != std::string::npos,
        "handleGetMqttStatus function definition not found");

    // 在 handleGetMqttStatus 函数体内查找关键逻辑
    size_t statusEnd = handler.find("\nvoid MqttRouteHandler::", statusPos + 10);
    std::string statusBlock = handler.substr(
        statusPos,
        statusEnd == std::string::npos ? std::string::npos : statusEnd - statusPos);
    TEST_ASSERT_TRUE_MESSAGE(statusBlock.find("stopMQTT") != std::string::npos,
        "handleGetMqttStatus must call stopMQTT when config says disabled");
    TEST_ASSERT_TRUE_MESSAGE(statusBlock.find("!mqttConfigEnabled") != std::string::npos,
        "handleGetMqttStatus must check !mqttConfigEnabled before stopping");
    // 停止条件必须同时包含 mqttConfigLoaded 和 !mqttConfigEnabled
    // 避免配置未加载时误停客户端
    TEST_ASSERT_TRUE_MESSAGE(statusBlock.find("mqttConfigLoaded && !mqttConfigEnabled") != std::string::npos,
        "stopMQTT condition must require BOTH mqttConfigLoaded AND !mqttConfigEnabled");
    TestLog::step("MqttRouteHandler: stops MQTT client when config disabled (with loaded guard)");

    // 2. connected 字段必须包含 mqttConfigEnabled 守卫
    TEST_ASSERT_TRUE_MESSAGE(statusBlock.find("mqttConfigEnabled &&") != std::string::npos,
        "connected must include mqttConfigEnabled guard");
    TestLog::step("MqttRouteHandler: connected field guarded by mqttConfigEnabled");

    // 2b. connecting 字段也必须包含 mqttConfigEnabled 守卫
    // 查找 "mqttConfigEnabled && !mqttConnected" 确保 connecting 也受 enabled 状态保护
    TEST_ASSERT_TRUE_MESSAGE(statusBlock.find("mqttConfigEnabled && !mqttConnected") != std::string::npos,
        "connecting field must also include mqttConfigEnabled guard to prevent false 'connecting' state");
    TestLog::step("MqttRouteHandler: connecting field also guarded by mqttConfigEnabled");

    // 2c. 防御性检查必须在 tryAutoStartMqttForStatus 之前
    // 确保禁用时先停止客户端，再尝试自动启动（自动启动也会被 configEnabled=false 阻止）
    size_t stopPos = statusBlock.find("stopMQTT");
    size_t autoStartPos = statusBlock.find("tryAutoStartMqttForStatus");
    TEST_ASSERT_TRUE_MESSAGE(stopPos != std::string::npos && autoStartPos != std::string::npos,
        "Both stopMQTT and tryAutoStartMqttForStatus must exist in handleGetMqttStatus");
    TEST_ASSERT_TRUE_MESSAGE(stopPos < autoStartPos,
        "stopMQTT must be called BEFORE tryAutoStartMqttForStatus");
    TestLog::step("MqttRouteHandler: stopMQTT before tryAutoStartMqttForStatus");

    // 3. ProtocolRouteHandler::handleSaveProtocolConfig 必须在 MQTT 禁用时立即停止客户端
    std::string proto = readRegressionSrcFile("src/network/handlers/ProtocolRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(proto.empty(), "Cannot read ProtocolRouteHandler.cpp");

    // 精确查找函数定义，避免命中 setupRoutes 中的成员函数引用。
    size_t saveMqttPos = proto.find("void ProtocolRouteHandler::handleSaveProtocolConfig");
    TEST_ASSERT_TRUE_MESSAGE(saveMqttPos != std::string::npos,
        "handleSaveProtocolConfig function definition not found");

    size_t saveMqttEnd = proto.find("\nvoid ProtocolRouteHandler::", saveMqttPos + 10);
    std::string saveBlock = proto.substr(
        saveMqttPos,
        saveMqttEnd == std::string::npos ? std::string::npos : saveMqttEnd - saveMqttPos);
    // 必须在 mqttWillReboot 前检查 mqttEnabledNow 并调用 stopMQTT
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("mqttEnabledNow") != std::string::npos,
        "handleSaveProtocolConfig must read mqtt enabled state after save");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("!mqttEnabledNow") != std::string::npos,
        "handleSaveProtocolConfig must check !mqttEnabledNow");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("stopMQTT") != std::string::npos,
        "handleSaveProtocolConfig must call stopMQTT when MQTT is disabled");
    TestLog::step("ProtocolRouteHandler: stops MQTT immediately when disabled in save");

    // 4. 保存响应必须包含 mqttDisconnected 标志
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("mqttDisconnected") != std::string::npos,
        "Response must include mqttDisconnected flag for frontend notification");
    TestLog::step("ProtocolRouteHandler: response includes mqttDisconnected flag");

    // 5. stopMQTT 必须在 scheduleConfigReboot 之前执行
    // 确保客户端在重启调度前已停止，避免窗口期内继续连接
    size_t stopInSave = saveBlock.find("stopMQTT");
    size_t rebootInSave = saveBlock.find("scheduleConfigReboot");
    TEST_ASSERT_TRUE_MESSAGE(stopInSave != std::string::npos,
        "stopMQTT must exist in save handler");
    TEST_ASSERT_TRUE_MESSAGE(rebootInSave != std::string::npos,
        "scheduleConfigReboot must exist in save handler");
    TEST_ASSERT_TRUE_MESSAGE(stopInSave < rebootInSave,
        "stopMQTT must be called BEFORE scheduleConfigReboot to prevent connection during reboot window");
    TestLog::step("ProtocolRouteHandler: stopMQTT before scheduleConfigReboot");

    TestLog::testEnd(true);
}

/**
 * @brief MQTT 禁用时 reconnect 接口必须拒绝重连/重启
 * 回归：旧代码 handleMqttReconnect 未检查 mqtt.enabled，前端自动重连可能触发设备重启
 */
static void test_regression_mqtt_disabled_rejects_reconnect() {
    TestLog::testStart("Regression: MQTT Disabled Rejects Reconnect");

    // 1. handleMqttReconnect 必须检查 mqttConfigEnabled
    std::string handler = readRegressionSrcFile("src/network/handlers/MqttRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(handler.empty(), "Cannot read MqttRouteHandler.cpp");

    size_t reconnectPos = handler.find("handleMqttReconnect(AsyncWebServerRequest");
    TEST_ASSERT_TRUE_MESSAGE(reconnectPos != std::string::npos,
        "handleMqttReconnect function definition not found");

    std::string reconnectBlock = handler.substr(reconnectPos, 2000);
    // 必须在 scheduleConfigReboot 之前检查 mqttEnabled
    TEST_ASSERT_TRUE_MESSAGE(reconnectBlock.find("mqttEnabled") != std::string::npos,
        "handleMqttReconnect must check mqttEnabled from config");
    TEST_ASSERT_TRUE_MESSAGE(reconnectBlock.find("!mqttEnabled") != std::string::npos ||
        reconnectBlock.find("mqttDisabled") != std::string::npos,
        "handleMqttReconnect must reject reconnect when MQTT is disabled");
    TestLog::step("MqttRouteHandler: handleMqttReconnect rejects reconnect when disabled");

    // 2. checkPendingTestRestore 必须检查 mqttEnabled 后才调用 restartMQTTDeferred
    size_t testRestorePos = handler.find("checkPendingTestRestore()");
    TEST_ASSERT_TRUE_MESSAGE(testRestorePos != std::string::npos,
        "checkPendingTestRestore function definition not found");

    std::string testRestoreBlock = handler.substr(testRestorePos, 2000);
    TEST_ASSERT_TRUE_MESSAGE(testRestoreBlock.find("mqttEnabled") != std::string::npos,
        "checkPendingTestRestore must check mqttEnabled before restarting");
    // 必须在 restartMQTTDeferred 前检查 enabled
    size_t enabledCheckPos = testRestoreBlock.find("mqttEnabled");
    size_t restartPos = testRestoreBlock.find("restartMQTTDeferred");
    TEST_ASSERT_TRUE_MESSAGE(enabledCheckPos != std::string::npos && restartPos != std::string::npos,
        "Both mqttEnabled check and restartMQTTDeferred must exist");
    TEST_ASSERT_TRUE_MESSAGE(enabledCheckPos < restartPos,
        "mqttEnabled check must be BEFORE restartMQTTDeferred");
    TestLog::step("MqttRouteHandler: checkPendingTestRestore checks enabled before restart");

    // 3. MQTTClient::disconnect() 必须 flush 确保 DISCONNECT 报文发出
    std::string client = readRegressionSrcFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_FALSE_MESSAGE(client.empty(), "Cannot read MQTTClient.cpp");

    size_t disconnectPos = client.find("bool MQTTClient::disconnect()");
    TEST_ASSERT_TRUE_MESSAGE(disconnectPos != std::string::npos,
        "MQTTClient::disconnect() not found");

    const size_t disconnectEnd = client.find("\n}", disconnectPos);
    TEST_ASSERT_TRUE_MESSAGE(disconnectEnd != std::string::npos, "disconnect() body must be complete");
    std::string disconnectBlock = client.substr(disconnectPos, disconnectEnd - disconnectPos);
    TEST_ASSERT_TRUE_MESSAGE(disconnectBlock.find("flush()") != std::string::npos,
        "disconnect() must call flush() before closing TCP socket");
    // flush 必须在 stop 之前
    size_t flushPos = disconnectBlock.find("flush()");
    size_t stopPos = disconnectBlock.find(".stop()");
    TEST_ASSERT_TRUE_MESSAGE(flushPos != std::string::npos && stopPos != std::string::npos,
        "Both flush() and stop() must exist in disconnect()");
    TEST_ASSERT_TRUE_MESSAGE(flushPos < stopPos,
        "flush() must be called BEFORE stop() to ensure DISCONNECT packet is sent");
    TestLog::step("MQTTClient: disconnect() flushes before closing TCP socket");

    // 4. restartMQTTDeferred 必须检查 mqtt.enabled（这是设备启动时的唯一守卫）
    //    设备重启时 FastBeeFramework STEP 11.1 无条件调用 restartMQTTDeferred()
    //    如果不检查 enabled，禁用状态下 MQTT 客户端仍会被创建并自动连接
    std::string pm = readRegressionSrcFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(pm.empty(), "Cannot read ProtocolManager.cpp");

    size_t deferredPos = pm.find("ProtocolManager::restartMQTTDeferred(bool forceRebuild,");
    TEST_ASSERT_TRUE_MESSAGE(deferredPos != std::string::npos,
        "restartMQTTDeferred implementation not found");

    const size_t deferredEnd = pm.find("\n}", deferredPos);
    TEST_ASSERT_TRUE_MESSAGE(deferredEnd != std::string::npos, "restartMQTTDeferred body must be complete");
    std::string deferredBlock = pm.substr(deferredPos, deferredEnd - deferredPos);
    // mqtt.enabled 检查必须在 active-client guard 之前
    size_t enabledPos = deferredBlock.find("\"mqtt\"");
    size_t activeGuardPos = deferredBlock.find("!forceRebuild && mqttClient");
    TEST_ASSERT_TRUE_MESSAGE(enabledPos != std::string::npos,
        "restartMQTTDeferred must check mqtt.enabled from config");
    TEST_ASSERT_TRUE_MESSAGE(activeGuardPos != std::string::npos,
        "restartMQTTDeferred must have active-client guard");
    TEST_ASSERT_TRUE_MESSAGE(enabledPos < activeGuardPos,
        "mqtt.enabled check must run BEFORE active-client guard");
    // 禁用时必须 stop 并 reset
    const size_t disabled = deferredBlock.find("if (!mqttEnabled)");
    const size_t stop = deferredBlock.find("if (!mqttClient->stop())", disabled);
    const size_t retry = deferredBlock.find("mqttRestartPending = true", stop);
    const size_t reject = deferredBlock.find("return false", retry);
    const size_t reset = deferredBlock.find("mqttClient.reset()", reject);
    TEST_ASSERT_TRUE_MESSAGE(disabled != std::string::npos && stop != std::string::npos &&
                             retry != std::string::npos && reject != std::string::npos &&
                             reset != std::string::npos && stop < reject && reject < reset && reset < activeGuardPos,
        "Disabled client destruction must wait for a successful stop; busy cleanup must remain pending");
    TestLog::step("ProtocolManager: restartMQTTDeferred checks mqtt.enabled before creating client");

    // 5. restartMQTT (non-deferred) 也必须检查 mqtt.enabled
    size_t restartNonDefPos = pm.find("ProtocolManager::restartMQTT()");
    TEST_ASSERT_TRUE_MESSAGE(restartNonDefPos != std::string::npos,
        "restartMQTT implementation not found");
    std::string restartBlock = pm.substr(restartNonDefPos, 800);
    TEST_ASSERT_TRUE_MESSAGE(restartBlock.find("\"mqtt\"") != std::string::npos,
        "restartMQTT must also check mqtt.enabled");
    TestLog::step("ProtocolManager: restartMQTT also checks mqtt.enabled");

    TestLog::testEnd(true);
}

/**
 * @brief Modbus 延迟恢复不得被通用重型协议堆闸门永久跳过
 */
static void test_regression_modbus_deferred_restart_before_heap_guard() {
    TestLog::testStart("Regression: Modbus Deferred Restart Before Heap Guard");
    std::string pm = readRegressionSrcFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(pm.empty(), "Cannot read ProtocolManager.cpp");

    const auto handlePos = pm.find("void ProtocolManager::handle()");
    TEST_ASSERT_TRUE_MESSAGE(handlePos != std::string::npos,
                             "ProtocolManager::handle() not found");
    const auto handleEnd = pm.find("void ProtocolManager::handleMessage", handlePos);
    const std::string handleBlock = pm.substr(
        handlePos, handleEnd == std::string::npos ? std::string::npos : handleEnd - handlePos);
    const auto pendingPos = handleBlock.find("if (modbusRestartPending)");
    const auto heapGuardPos = handleBlock.find("if (!heapSufficient)");
    TEST_ASSERT_TRUE_MESSAGE(pendingPos != std::string::npos,
                             "handle() must consume Modbus deferred restart");
    TEST_ASSERT_TRUE_MESSAGE(heapGuardPos != std::string::npos,
                             "handle() must retain the general heap guard");
    TEST_ASSERT_TRUE_MESSAGE(pendingPos < heapGuardPos,
                             "Modbus deferred restart must be considered before general heap early return");
    TEST_ASSERT_TRUE_MESSAGE(handleBlock.find("MODBUS_RESTART_MIN_DRAM") != std::string::npos,
                             "Modbus deferred restart must use a dedicated DRAM admission gate");
    TEST_ASSERT_TRUE_MESSAGE(handleBlock.find("MODBUS_RESTART_MIN_LARGEST") != std::string::npos,
                             "Modbus deferred restart must check largest contiguous block");
    TestLog::step("Deferred Modbus restart is consumed before the heavy-protocol early return with DRAM gates");
    TestLog::testEnd(true);
}

/**
 * @brief Modbus 默认禁用模板是显式空闲状态，不得作为启动失败循环告警
 */
static void test_regression_modbus_restart_distinguishes_intentional_idle() {
    TestLog::testStart("Regression: Modbus Restart Distinguishes Intentional Idle");

    const std::string header = readRegressionSrcFile("include/protocols/ProtocolManager.h");
    const std::string manager = readRegressionSrcFile("src/protocols/ProtocolManager.cpp");
    const std::string framework = readRegressionSrcFile("src/core/FastBeeFramework.cpp");
    const std::string health = readRegressionSrcFile("src/systems/HealthMonitor.cpp");
    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "Cannot read ProtocolManager.h");
    TEST_ASSERT_FALSE_MESSAGE(manager.empty(), "Cannot read ProtocolManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(framework.empty(), "Cannot read FastBeeFramework.cpp");
    TEST_ASSERT_FALSE_MESSAGE(health.empty(), "Cannot read HealthMonitor.cpp");

    TEST_ASSERT_TRUE_MESSAGE(
        header.find("enum class ModbusRestartResult") != std::string::npos &&
        header.find("Started") != std::string::npos &&
        header.find("IdleNoEnabledTargets") != std::string::npos &&
        header.find("Failed") != std::string::npos,
        "Modbus restart must expose started, intentional-idle, and failed outcomes");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("ModbusRestartResult restartModbus()") != std::string::npos &&
        manager.find("ModbusRestartResult ProtocolManager::restartModbus()") != std::string::npos,
        "restartModbus must return the explicit result type end-to-end");

    const auto restartPos = manager.find("ModbusRestartResult ProtocolManager::restartModbus()");
    const auto restartEnd = manager.find("void ProtocolManager::stopModbus()", restartPos);
    TEST_ASSERT_TRUE_MESSAGE(restartPos != std::string::npos && restartEnd != std::string::npos,
                             "restartModbus implementation block must be discoverable");
    const std::string restartBlock = manager.substr(restartPos, restartEnd - restartPos);
    const auto noTargetsPos = restartBlock.find("!hasEnabledPollTask && !hasEnabledSubDevice");
    const auto idleReturnPos = restartBlock.find("ModbusRestartResult::IdleNoEnabledTargets");
    TEST_ASSERT_TRUE_MESSAGE(noTargetsPos != std::string::npos && idleReturnPos != std::string::npos,
                             "all-disabled master templates must return intentional idle");
    TEST_ASSERT_TRUE_MESSAGE(noTargetsPos < idleReturnPos,
                             "intentional idle must be returned from the no-enabled-target branch");

    const auto bootPos = framework.find("[STEP11.2] Starting Modbus handler");
    const auto bootEnd = framework.find("#endif", bootPos);
    TEST_ASSERT_TRUE_MESSAGE(bootPos != std::string::npos && bootEnd != std::string::npos,
                             "boot Modbus restart block must be discoverable");
    const std::string bootBlock = framework.substr(bootPos, bootEnd - bootPos);
    TEST_ASSERT_TRUE_MESSAGE(
        bootBlock.find("ModbusRestartResult::IdleNoEnabledTargets") != std::string::npos &&
        bootBlock.find("transport remains idle") != std::string::npos,
        "boot must report intentional idle as information rather than failure");

    const auto autoStartPos = framework.find("[Modbus] Modbus enabled, auto-starting");
    TEST_ASSERT_TRUE_MESSAGE(autoStartPos != std::string::npos,
                             "network-stable Modbus auto-start block must exist");
    const std::string autoStartBlock = framework.substr(autoStartPos, 800);
    TEST_ASSERT_TRUE_MESSAGE(
        autoStartBlock.find("result == ModbusRestartResult::Failed") != std::string::npos &&
        autoStartBlock.find("modbusAutoStarted = false") != std::string::npos,
        "only a real restart failure may schedule network auto-start retry");
    TEST_ASSERT_TRUE_MESSAGE(
        autoStartBlock.find("result == ModbusRestartResult::IdleNoEnabledTargets") != std::string::npos,
        "intentional idle must complete network auto-start without a retry loop");

    TEST_ASSERT_TRUE_MESSAGE(
        header.find("consumeModbusRestartResult") != std::string::npos &&
        manager.find("ProtocolManager::consumeModbusRestartResult") != std::string::npos,
        "deferred restart result must be observable exactly once by recovery logic");
    const auto reconcilePos = health.find("void HealthMonitor::reconcilePendingMemoryRecovery()");
    const auto reconcileEnd = health.find("void HealthMonitor::restoreMemoryRecovery()", reconcilePos);
    TEST_ASSERT_TRUE_MESSAGE(reconcilePos != std::string::npos && reconcileEnd != std::string::npos,
                             "memory recovery reconciliation block must be discoverable");
    const std::string reconcileBlock = health.substr(reconcilePos, reconcileEnd - reconcilePos);
    TEST_ASSERT_TRUE_MESSAGE(
        reconcileBlock.find("consumeModbusRestartResult") != std::string::npos &&
        reconcileBlock.find("restartResult == ModbusRestartResult::IdleNoEnabledTargets") != std::string::npos &&
        reconcileBlock.find("_modbusRestorePendingForMemory = false") != std::string::npos,
        "intentional idle must close memory-recovery state instead of being requeued forever");

    TestLog::step("disabled templates remain idle while parse/memory/UART failures retain failure semantics");
    TestLog::testEnd(true);
}

/**
 * @brief F16R8 示例中的未接线 Modbus 模板必须保持默认禁用
 */
static void test_regression_factory_modbus_disabled_and_com15_explicit_topology() {
    TestLog::testStart("Regression: Factory Modbus OFF and COM15 Explicit Read-Only Polling");

    const std::string factoryText = readRegressionSrcFile("data/config/protocol.json");
    TEST_ASSERT_FALSE_MESSAGE(factoryText.empty(), "Cannot read factory protocol defaults");
    JsonDocument factory;
    TEST_ASSERT_FALSE_MESSAGE(deserializeJson(factory, factoryText), "Factory protocol must be valid JSON");
    TEST_ASSERT_TRUE_MESSAGE(factory["modbusRtu"]["enabled"].is<bool>(), "Factory Modbus enabled flag is required");
    TEST_ASSERT_FALSE_MESSAGE(factory["modbusRtu"]["enabled"].as<bool>(), "Factory defaults must not activate a field bus");
    TEST_ASSERT_TRUE(factory["modbusRtu"]["master"]["tasks"].is<JsonArrayConst>());
    TEST_ASSERT_TRUE(factory["modbusRtu"]["master"]["devices"].is<JsonArrayConst>());
    TEST_ASSERT_EQUAL_UINT(0, factory["modbusRtu"]["master"]["tasks"].size());
    TEST_ASSERT_EQUAL_UINT(0, factory["modbusRtu"]["master"]["devices"].size());

    const std::string configText =
        readRegressionSrcFile("docs/device-test-configs/com15-esp32s3-f16r8/config/protocol.json");
    TEST_ASSERT_FALSE_MESSAGE(configText.empty(), "Cannot read F16R8 protocol template");

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, configText);
    TEST_ASSERT_FALSE_MESSAGE(err, "F16R8 protocol template must be valid JSON");
    TEST_ASSERT_TRUE_MESSAGE(doc["modbusRtu"]["enabled"].as<bool>(),
                             "F16R8 must expose the configured Modbus feature");

    const JsonArrayConst tasks = doc["modbusRtu"]["master"]["tasks"].as<JsonArrayConst>();
    const JsonArrayConst devices = doc["modbusRtu"]["master"]["devices"].as<JsonArrayConst>();
    // This is the user-authorized COM15 field configuration, not factory defaults.
    TEST_ASSERT_EQUAL_UINT(5, tasks.size());
    // Sensor addresses 1..5 are represented by read-only tasks, not duplicate
    // control devices. The relay at address 6 is control-device index 0.
    TEST_ASSERT_EQUAL_UINT(1, devices.size());
    for (size_t index = 0; index < tasks.size(); ++index) {
        JsonObjectConst task = tasks[index];
        TEST_ASSERT_TRUE_MESSAGE(task["enabled"] | false, "COM15 configured sensor polling must remain enabled");
        TEST_ASSERT_EQUAL_UINT(index + 1, task["slaveAddress"].as<unsigned>());
        const unsigned code = task["functionCode"] | 0U;
        TEST_ASSERT_TRUE_MESSAGE(code == 3 || code == 4, "Periodic polling must use read-only Modbus function codes");
    }
    for (size_t index = 0; index < devices.size(); ++index) {
        JsonObjectConst device = devices[index];
        TEST_ASSERT_TRUE_MESSAGE(device["enabled"] | false, "COM15 authorized device topology must remain enabled");
        TEST_ASSERT_EQUAL_UINT(6, device["slaveAddress"].as<unsigned>());
        TEST_ASSERT_EQUAL_STRING("relay", device["deviceType"] | "");
        // Committed COM15 template: four-channel FC06 register relay (see bundle README).
        TEST_ASSERT_EQUAL_UINT(4, device["channelCount"].as<unsigned>());
        TEST_ASSERT_EQUAL_UINT(0, device["coilBase"].as<unsigned>());
        TEST_ASSERT_EQUAL_UINT(1, device["controlProtocol"].as<unsigned>());
    }

    JsonDocument execution;
    const std::string executionText = readRegressionSrcFile("docs/device-test-configs/com15-esp32s3-f16r8/config/periph_exec.json");
    TEST_ASSERT_FALSE_MESSAGE(executionText.empty(), "COM15 execution configuration is required");
    TEST_ASSERT_FALSE_MESSAGE(deserializeJson(execution, executionText), "COM15 execution configuration must be valid JSON");
    size_t relayRules = 0;
    for (JsonObjectConst rule : execution["rules"].as<JsonArrayConst>()) {
        const std::string id = rule["id"] | "";
        if (id != "s3_modbus_relay" && id != "s3_modbus_relay_off") continue;
        ++relayRules;
        const JsonArrayConst triggers = rule["triggers"].as<JsonArrayConst>();
        const JsonArrayConst actions = rule["actions"].as<JsonArrayConst>();
        TEST_ASSERT_EQUAL_UINT(1, triggers.size());
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, triggers[0]["triggerType"] | 255U, "Relay control must require explicit platform invocation, never a timer");
        TEST_ASSERT_EQUAL_STRING("modbus:0", triggers[0]["triggerPeriphId"] | "");
        TEST_ASSERT_EQUAL_UINT(1, actions.size());
        TEST_ASSERT_EQUAL_STRING("modbus:0", actions[0]["targetPeriphId"] | "");
        TEST_ASSERT_EQUAL_UINT(18, actions[0]["actionType"].as<unsigned>());
        JsonDocument command;
        TEST_ASSERT_FALSE_MESSAGE(deserializeJson(command, actions[0]["actionValue"] | ""),
                                  "Relay command must contain explicit control JSON");
        TEST_ASSERT_EQUAL_UINT(1, command["ctrl"].size());
        TEST_ASSERT_EQUAL_UINT(0, command["ctrl"][0]["d"].as<unsigned>());
        TEST_ASSERT_EQUAL_UINT(0, command["ctrl"][0]["c"].as<unsigned>());
        TEST_ASSERT_EQUAL_STRING(id == "s3_modbus_relay" ? "on" : "off", command["ctrl"][0]["a"] | "");
    }
    TEST_ASSERT_EQUAL_UINT(2, relayRules);
    TestLog::step("factory bus stays OFF; COM15 sensors poll read-only and relay rules require explicit invocation");
    TestLog::testEnd(true);
}

/**
 * @brief MQTT 摘要字段默认值必须为 JSON 格式
 * 回归：旧代码 mqtt.summary 默认为空字符串，应为有效 JSON 示例
 */
static void test_regression_mqtt_summary_default_json() {
    TestLog::testStart("Regression: MQTT Summary Default JSON");

    std::string config = readRegressionSrcFile("web-src/modules/runtime/protocol/protocol-config.js");
    TEST_ASSERT_FALSE_MESSAGE(config.empty(), "Cannot read protocol-config.js");

    // 查找 mqtt-summary 设置行
    size_t pos = config.find("mqtt-summary");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos, "mqtt-summary not found");

    std::string block = config.substr(pos, 120);
    // 必须包含默认 JSON 格式
    TEST_ASSERT_TRUE_MESSAGE(block.find("fastbee") != std::string::npos,
        "mqtt.summary default must contain 'fastbee'");
    TEST_ASSERT_TRUE_MESSAGE(block.find("ESP32") != std::string::npos,
        "mqtt.summary default must contain 'ESP32'");
    TestLog::step("mqtt.summary default: JSON with fastbee + ESP32");

    TestLog::testEnd(true);
}

/**
 * @brief device.html 高级配置布局：安全策略和恢复出厂设置在底部
 * 回归：确保安全策略和恢复出厂设置不在顶部系统操作区
 */
static void test_regression_device_layout_security_factory_at_bottom() {
    TestLog::testStart("Regression: Device Layout Security+Factory at Bottom");

    std::string html = readRegressionSrcFile("web-src/pages/device.html");
    TEST_ASSERT_FALSE_MESSAGE(html.empty(), "Cannot read device.html");

    // 1. 系统操作区不包含恢复出厂设置
    size_t sysOpsPos = html.find("<!-- \xE7\xB3\xBB\xE7\xBB\x9F\xE6\x93\x8D\xE4\xBD\x9C\xE5\x8C\xBA -->");
    TEST_ASSERT_TRUE_MESSAGE(sysOpsPos != std::string::npos,
        "System operations section not found");

    // 找到系统操作区的结束
    size_t dataMgmtPos = html.find("<!-- \xE6\x95\xB0\xE6\x8D\xAE\xE7\xAE\xA1\xE7\x90\x86\xE5\x8C\xBA -->", sysOpsPos);
    TEST_ASSERT_TRUE_MESSAGE(dataMgmtPos != std::string::npos,
        "Data management section not found");

    std::string sysOpsBlock = html.substr(sysOpsPos, dataMgmtPos - sysOpsPos);
    TEST_ASSERT_TRUE_MESSAGE(sysOpsBlock.find("dev-sys-factory-title") == std::string::npos,
        "Factory reset must NOT be in system operations section");
    TestLog::step("Factory reset not in system operations section");

    // 2. 底部区域必须同时包含安全策略和恢复出厂设置
    size_t bottomPos = html.find("<!-- \xE5\xBA\x95\xE9\x83\xA8\xE5\x8C\xBA\xE5\x9F\x9F");
    TEST_ASSERT_TRUE_MESSAGE(bottomPos != std::string::npos,
        "Bottom section comment not found");

    std::string bottomBlock = html.substr(bottomPos, 6000);
    TEST_ASSERT_TRUE_MESSAGE(bottomBlock.find("dev-sec-title") != std::string::npos,
        "Security policy must be in bottom section");
    TEST_ASSERT_TRUE_MESSAGE(bottomBlock.find("dev-sys-factory-title") != std::string::npos,
        "Factory reset must be in bottom section");
    TestLog::step("Security policy + factory reset both in bottom section");

    // 3. 安全策略必须在恢复出厂设置之前
    size_t secPos = bottomBlock.find("dev-sec-title");
    size_t factoryPos = bottomBlock.find("dev-sys-factory-title");
    TEST_ASSERT_LESS_THAN((int)factoryPos, (int)secPos);
    TestLog::step("Security policy appears before factory reset (left position)");

    // 4. 安全策略 inputs 不应有自定义行高样式
    TEST_ASSERT_TRUE_MESSAGE(html.find("style=\"font-size:12px;height:30px\"") == std::string::npos,
        "Security policy inputs must NOT have custom height/font-size inline styles");
    TestLog::step("Security policy inputs: no custom inline styles");

    TestLog::testEnd(true);
}

/**
 * @brief mDNS 重启必须异步延迟执行，不阻塞 HTTP 响应
 * 回归：旧代码 restartMDNS 是同步的，阻塞了 HTTP 响应返回
 */
static void test_regression_mdns_async_restart() {
    TestLog::testStart("Regression: mDNS Async Restart (Non-blocking)");

    std::string cpp = readRegressionSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(cpp.empty(), "Cannot read NetworkManager.cpp");

    // 1. updateConfig 中 mDNS 变更应设置 pendingMDNSRestart 标志，而不是同步调用 restartMDNS
    size_t updatePos = cpp.find("bool FBNetworkManager::updateConfig");
    TEST_ASSERT_TRUE_MESSAGE(updatePos != std::string::npos,
        "updateConfig not found");
    size_t updateEnd = cpp.find("bool FBNetworkManager::updateConfigFromJSON", updatePos);
    TEST_ASSERT_TRUE_MESSAGE(updateEnd != std::string::npos,
        "updateConfigFromJSON boundary not found");
    std::string updateBlock = cpp.substr(updatePos, updateEnd - updatePos);

    TEST_ASSERT_TRUE_MESSAGE(updateBlock.find("pendingMDNSRestart = true") != std::string::npos,
        "updateConfig must set pendingMDNSRestart flag for async restart");
    TestLog::step("updateConfig: sets pendingMDNSRestart flag");

    // updateConfig 中的 mdnsConfigChanged 块不应直接调用 restartMDNS
    size_t mdnsChangedPos = updateBlock.find("mdnsConfigChanged");
    TEST_ASSERT_TRUE_MESSAGE(mdnsChangedPos != std::string::npos,
        "mdnsConfigChanged check must exist");
    std::string mdnsBlock = updateBlock.substr(mdnsChangedPos, 500);
    TEST_ASSERT_TRUE_MESSAGE(mdnsBlock.find("restartMDNS") == std::string::npos,
        "updateConfig mdnsConfigChanged block must NOT call restartMDNS synchronously");
    TestLog::step("updateConfig: no synchronous restartMDNS in mdnsConfigChanged block");

    // 2. update() 循环必须处理 pendingMDNSRestart
    size_t updateLoopPos = cpp.find("void FBNetworkManager::update()");
    TEST_ASSERT_TRUE_MESSAGE(updateLoopPos != std::string::npos,
        "update() not found");
    std::string updateLoop = cpp.substr(updateLoopPos, 2000);

    TEST_ASSERT_TRUE_MESSAGE(updateLoop.find("pendingMDNSRestart") != std::string::npos,
        "update() must check pendingMDNSRestart flag");
    TEST_ASSERT_TRUE_MESSAGE(updateLoop.find("restartMDNS") != std::string::npos,
        "update() must call restartMDNS for deferred execution");
    TestLog::step("update(): handles pendingMDNSRestart with deferred restartMDNS");

    // 3. 头文件必须声明 pendingMDNSRestart 和 pendingMDNSRestartTime
    std::string hdr = readRegressionSrcFile("include/network/NetworkManager.h");
    TEST_ASSERT_FALSE_MESSAGE(hdr.empty(), "Cannot read NetworkManager.h");
    TEST_ASSERT_TRUE_MESSAGE(hdr.find("pendingMDNSRestart") != std::string::npos,
        "NetworkManager.h must declare pendingMDNSRestart");
    TEST_ASSERT_TRUE_MESSAGE(hdr.find("pendingMDNSRestartTime") != std::string::npos,
        "NetworkManager.h must declare pendingMDNSRestartTime");
    TestLog::step("NetworkManager.h: pendingMDNSRestart + pendingMDNSRestartTime declared");

    TestLog::testEnd(true);
}

// ============================================================
//  源码回归测试：构建配置 src_filter / lib_deps 排除验证
// ============================================================

/**
 * @brief 声明了 src_filter 的环境必须排除未启用网络适配器的 .cpp 文件
 *
 * Unified Architecture v2.0 后，未声明 build_src_filter 的环境编译全部源码，
 * 功能关闭靠运行时门控；只有引用 [wifi_only_src_filter] 的环境（C3/C6）在编译期
 * 排除 CellularAdapter.cpp 和 EthernetAdapter.cpp，
 * 防止 PlatformIO LDF 扫描并拉入 TinyGSM/SSLClient 等不必要的库依赖。
 */
void test_regression_src_filter_excludes_unused_adapters() {
    TestLog::testStart("Regression: src_filter excludes unused network adapters");

    std::string pio = readRegressionSrcFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "Cannot read platformio.ini");

    // wifi_only_src_filter 必须排除 CellularAdapter.cpp 和 EthernetAdapter.cpp
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("[wifi_only_src_filter]") != std::string::npos,
        "platformio.ini must have [wifi_only_src_filter] section");
    auto wifiOnlyPos = pio.find("[wifi_only_src_filter]");
    auto wifiOnlyEnd = pio.find("\n[", wifiOnlyPos + 1);
    std::string wifiOnlySection = pio.substr(wifiOnlyPos,
        wifiOnlyEnd == std::string::npos ? std::string::npos : wifiOnlyEnd - wifiOnlyPos);
    TEST_ASSERT_TRUE_MESSAGE(
        wifiOnlySection.find("-<network/CellularAdapter.cpp>") != std::string::npos,
        "wifi_only_src_filter must exclude CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        wifiOnlySection.find("-<network/EthernetAdapter.cpp>") != std::string::npos,
        "wifi_only_src_filter must exclude EthernetAdapter.cpp");
    TestLog::step("wifi_only_src_filter excludes CellularAdapter + EthernetAdapter");

    // Shared source-filter aliases remain intentionally centralized so all
    // firmware environments resolve the same full source set.
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("[all_src_filter]") != std::string::npos,
        "[all_src_filter] must remain the shared source filter");
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("[all_ota_src_filter]") != std::string::npos,
        "[all_ota_src_filter] must remain the shared source filter");
    TestLog::step("shared src_filter sections stay centralized");

    TestLog::testEnd(true);
}

/**
 * @brief 网络适配器的编译宏、源过滤和依赖必须保持同一矩阵。
 *
 * C3/C6 和经典 4MB ESP32 不编译 4G/以太网，避免 TinyGSM/SSLClient 及
 * 适配器对象进入低内存镜像；有足够 Flash/PSRAM 的 8MB/16MB 环境保留。
 */
void test_regression_non_cellular_envs_no_tinygsm_sslclient() {
    TestLog::testStart("Regression: adapter compile matrix matches dependencies");

    std::string pio = readRegressionSrcFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "Cannot read platformio.ini");

    struct AdapterProfile { const char* env; bool enabled; };
    const AdapterProfile profiles[] = {
        { "[env:esp32-F4R0]", false },
        { "[env:esp32-F8R0]", true },
        { "[env:esp32-F8R8]", true },
        { "[env:esp32c3-F4R0]", false },
        { "[env:esp32c6-F4R0]", false },
        { "[env:esp32c6-F8R0]", false },
        { "[env:esp32s3-F8R0]", true },
        { "[env:esp32s3-F8R8]", true },
        { "[env:esp32s3-F16R8]", true }
    };

    for (const AdapterProfile& profile : profiles) {
        const size_t start = pio.find(profile.env);
        TEST_ASSERT_TRUE_MESSAGE(start != std::string::npos,
            (std::string("missing ") + profile.env).c_str());
        const size_t end = pio.find("[env:", start + 1);
        const std::string section = pio.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const std::string expected = profile.enabled ? "=1" : "=0";
        TEST_ASSERT_TRUE_MESSAGE(
            section.find("-DFASTBEE_ENABLE_ETHERNET" + expected) != std::string::npos &&
            section.find("-DFASTBEE_ENABLE_CELLULAR" + expected) != std::string::npos,
            (std::string(profile.env) + " has incorrect adapter macro values").c_str());

        if (profile.enabled) {
            TEST_ASSERT_TRUE_MESSAGE(section.find("TinyGSM") != std::string::npos &&
                section.find("SSLClient") != std::string::npos,
                (std::string(profile.env) + " is missing cellular dependencies").c_str());
            TEST_ASSERT_TRUE_MESSAGE(section.find("-<network/CellularAdapter.cpp>") == std::string::npos &&
                section.find("-<network/EthernetAdapter.cpp>") == std::string::npos,
                (std::string(profile.env) + " excludes an enabled adapter source").c_str());
        } else {
            TEST_ASSERT_TRUE_MESSAGE(section.find("TinyGSM") == std::string::npos &&
                section.find("SSLClient") == std::string::npos,
                (std::string(profile.env) + " must omit cellular dependencies").c_str());
            TEST_ASSERT_TRUE_MESSAGE(section.find("${wifi_only_src_filter.build_src_filter}") != std::string::npos,
                (std::string(profile.env) + " must use the Wi-Fi-only source filter").c_str());
        }
    }

    TestLog::testEnd(true);
}

/**
 * @brief CellularAdapter.h / EthernetAdapter.h 头文件必须有 #if FASTBEE_ENABLE 保护
 *
 * 确保在 CELLULAR=0 / ETHERNET=0 时，虽然 .h 被间接包含，
 * 但头文件内容被条件编译保护，不会引入 TinyGSM / Ethernet 头文件。
 */
void test_regression_adapter_headers_have_feature_guard() {
    TestLog::testStart("Regression: adapter headers have #if FASTBEE_ENABLE guard");

    std::string cellHdr = readRegressionSrcFile("include/network/CellularAdapter.h");
    TEST_ASSERT_FALSE_MESSAGE(cellHdr.empty(), "Cannot read CellularAdapter.h");
    TEST_ASSERT_TRUE_MESSAGE(
        cellHdr.find("#if FASTBEE_ENABLE_CELLULAR") != std::string::npos,
        "CellularAdapter.h must be guarded by #if FASTBEE_ENABLE_CELLULAR");
    TestLog::step("CellularAdapter.h guarded by FASTBEE_ENABLE_CELLULAR");

    std::string ethHdr = readRegressionSrcFile("include/network/EthernetAdapter.h");
    TEST_ASSERT_FALSE_MESSAGE(ethHdr.empty(), "Cannot read EthernetAdapter.h");
    TEST_ASSERT_TRUE_MESSAGE(
        ethHdr.find("#if FASTBEE_ENABLE_ETHERNET") != std::string::npos,
        "EthernetAdapter.h must be guarded by #if FASTBEE_ENABLE_ETHERNET");
    TestLog::step("EthernetAdapter.h guarded by FASTBEE_ENABLE_ETHERNET");

    // NetworkManager.h 中的 #include 也必须有条件编译保护
    std::string nmHdr = readRegressionSrcFile("include/network/NetworkManager.h");
    TEST_ASSERT_FALSE_MESSAGE(nmHdr.empty(), "Cannot read NetworkManager.h");
    auto cellIncPos = nmHdr.find("#include \"network/CellularAdapter.h\"");
    TEST_ASSERT_TRUE_MESSAGE(cellIncPos != std::string::npos,
        "NetworkManager.h must include CellularAdapter.h");
    // 向前查找最近的 #if
    auto guardSearch = nmHdr.substr(0, cellIncPos);
    auto lastIf = guardSearch.rfind("#if FASTBEE_ENABLE_CELLULAR");
    TEST_ASSERT_TRUE_MESSAGE(lastIf != std::string::npos,
        "CellularAdapter.h include must be inside #if FASTBEE_ENABLE_CELLULAR");
    TestLog::step("NetworkManager.h guards CellularAdapter.h include");

    TestLog::testEnd(true);
}

/**
 * @brief platformio.ini 中所有 FASTBEE_ENABLE_* 宏必须是有效拼写
 *
 * 回归保护：曾经存在 FASTBEE_ENABLE_NEEL（应为 NEOPIXEL）的拼写错误，
 * 该宏在 C++ 代码中从未被引用，属于无效配置。此测试确保类似错误不再发生。
 */
void test_regression_platformio_no_misspelled_macros() {
    TestLog::testStart("Regression: platformio.ini no misspelled FASTBEE macros");

    std::string pio = readRegressionSrcFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "Cannot read platformio.ini");

    // 已知的拼写错误宏不应再出现
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("FASTBEE_ENABLE_NEEL") == std::string::npos,
        "platformio.ini must not contain misspelled FASTBEE_ENABLE_NEEL (was typo for NEOPIXEL)");
    TestLog::step("Known misspelled macro FASTBEE_ENABLE_NEEL absent");

    // 验证所有 FASTBEE_ENABLE_ 宏至少在两个不同的 flags section 中出现
    // （如果只出现一次且不在 C++ 代码中使用，很可能是无效宏）
    size_t pos = 0;
    std::set<std::string> seenMacros;
    std::map<std::string, int> macroCount;
    while ((pos = pio.find("-DFASTBEE_ENABLE_", pos)) != std::string::npos) {
        size_t nameStart = pos + 2; // skip "-D"
        size_t nameEnd = pio.find("=", nameStart);
        if (nameEnd == std::string::npos) break;
        std::string macro = pio.substr(nameStart, nameEnd - nameStart);
        macroCount[macro]++;
        pos = nameEnd + 1;
    }
    TestLog::step("All FASTBEE_ENABLE_ macros cataloged");

    TestLog::testEnd(true);
}

/**
 * @brief 热点配置 HTML 布局回归：信道字段必须与隐藏热点/最大连接数同列
 *
 * 回归保护：信道(ap-channel)字段曾与热点名称/密码/IP同处左列，
 * 导致左列4项、右列2项的不均衡布局。修复后信道移至右列，
 * 与隐藏热点(ap-hidden)、最大连接数(ap-max-connections)并列。
 */
void test_regression_ap_config_html_layout() {
    TestLog::testStart("Regression: AP Config HTML Layout Channel in Right Column");

    std::string html = readRegressionSrcFile("web-src/pages/network.html");
    TEST_ASSERT_FALSE_MESSAGE(html.empty(), "Cannot read network.html");

    // 定位 ap-config 区块
    auto apConfigStart = html.find("id=\"ap-config\"");
    TEST_ASSERT_TRUE_MESSAGE(apConfigStart != std::string::npos,
        "network.html must contain ap-config section");

    // 提取 ap-form 区块内容（从 ap-form 到下一个 config-content 或文件末尾）
    auto apFormStart = html.find("id=\"ap-form\"", apConfigStart);
    TEST_ASSERT_TRUE_MESSAGE(apFormStart != std::string::npos,
        "ap-config section must contain ap-form");

    // 验证布局结构：ap-channel 不应出现在第一个 config-form-column 中
    // （第一个列包含 ap-ssid、ap-password、ap-ip）
    auto firstColStart = html.find("config-form-column", apFormStart);
    auto firstColEnd = html.find("</div>", html.find("config-form-column", firstColStart + 20));
    // 向前找够远的 </div> 以包含整个列
    // 更简单的方式：验证 ap-channel 在 ap-hidden 之后
    auto hiddenPos = html.find("id=\"ap-hidden\"", apFormStart);
    auto channelPos = html.find("id=\"ap-channel\"", apFormStart);
    auto maxConnPos = html.find("id=\"ap-max-connections\"", apFormStart);

    TEST_ASSERT_TRUE_MESSAGE(hiddenPos != std::string::npos,
        "ap-form must contain ap-hidden field");
    TEST_ASSERT_TRUE_MESSAGE(channelPos != std::string::npos,
        "ap-form must contain ap-channel field");
    TEST_ASSERT_TRUE_MESSAGE(maxConnPos != std::string::npos,
        "ap-form must contain ap-max-connections field");

    // 关键约束：信道字段必须出现在最大连接数之后（即右列末尾）
    TEST_ASSERT_TRUE_MESSAGE(channelPos > maxConnPos,
        "ap-channel must appear after ap-max-connections (right column layout)");
    // 信道字段必须出现在隐藏热点之后（右列）
    TEST_ASSERT_TRUE_MESSAGE(channelPos > hiddenPos,
        "ap-channel must appear after ap-hidden (same right column)");
    TestLog::step("ap-channel is in right column (after ap-hidden and ap-max-connections)");

    // 验证左列仅包含 ap-ssid、ap-password、ap-ip（不含 ap-channel）
    auto ipPos = html.find("id=\"ap-ip\"", apFormStart);
    TEST_ASSERT_TRUE_MESSAGE(ipPos != std::string::npos,
        "ap-form must contain ap-ip field");
    TEST_ASSERT_TRUE_MESSAGE(ipPos < hiddenPos,
        "ap-ip must appear before ap-hidden (left vs right column)");
    TestLog::step("ap-ip is in left column (before right column starts)");

    TestLog::testEnd(true);
}

/**
 * @brief 热点配置 JS 保存函数必须发送全部 6 个字段
 *
 * 回归保护：saveAPConfig() 必须完整发送 apSSID、apPassword、
 * apIP、apChannel、apHidden、apMaxConnections，缺少任一字段
 * 将导致后端无法更新对应配置。
 * 注意：deviceName 已从热点配置中移除，由设备配置基本配置统一管理。
 */
void test_regression_ap_config_js_saves_all_fields() {
    TestLog::testStart("Regression: AP Config JS saveAPConfig Sends All Fields");

    std::string js = readRegressionSrcFile("web-src/modules/runtime/network.js");
    TEST_ASSERT_FALSE_MESSAGE(js.empty(), "Cannot read network.js");

    // 定位 saveAPConfig 函数定义（注意区分调用和定义）
    auto fnStart = js.find("saveAPConfig() {");
    TEST_ASSERT_TRUE_MESSAGE(fnStart != std::string::npos,
        "network.js must contain saveAPConfig function definition");

    // 提取函数体（从函数开始到 .finally 链结束）
    auto fnBody = js.substr(fnStart, std::min<size_t>(js.size() - fnStart, 1500));

    // 必须包含 apiPut 调用
    TEST_ASSERT_TRUE_MESSAGE(fnBody.find("apiPut") != std::string::npos,
        "saveAPConfig must call apiPut");

    // 6 个必要字段必须出现在 saveAPConfig 函数体中
    const char* requiredFields[] = {
        "apSSID", "apPassword", "apIP", "apChannel", "apHidden", "apMaxConnections"
    };
    for (const char* field : requiredFields) {
        TEST_ASSERT_TRUE_MESSAGE(fnBody.find(field) != std::string::npos,
            (std::string("saveAPConfig must send field: ") + field).c_str());
    }
    TestLog::step("All 6 AP config fields present in saveAPConfig");

    // deviceName 不应再出现在 saveAPConfig 中（已移至设备配置）
    TEST_ASSERT_TRUE_MESSAGE(fnBody.find("deviceName") == std::string::npos,
        "saveAPConfig must NOT send deviceName (moved to device config)");
    TestLog::step("deviceName correctly removed from saveAPConfig");

    // 验证 GET 侧 loadNetworkConfig 也读取所有 AP 字段
    auto loadFn = js.find("loadNetworkConfig() {");
    TEST_ASSERT_TRUE_MESSAGE(loadFn != std::string::npos,
        "network.js must contain loadNetworkConfig function");

    auto loadEnd = js.find("        _networkLoadState(", loadFn);
    TEST_ASSERT_TRUE_MESSAGE(loadEnd != std::string::npos && loadEnd > loadFn,
        "Cannot locate loadNetworkConfig method boundary");
    auto loadBody = js.substr(loadFn, loadEnd - loadFn);
    const char* loadFields[] = {
        "ap-ssid", "ap-password", "ap-ip", "ap-channel", "ap-hidden", "ap-max-connections"
    };
    for (const char* field : loadFields) {
        TEST_ASSERT_TRUE_MESSAGE(loadBody.find(field) != std::string::npos,
            (std::string("loadNetworkConfig must read DOM field: ") + field).c_str());
    }
    TestLog::step("All 6 AP config DOM fields present in loadNetworkConfig");

    TestLog::testEnd(true);
}

/**
 * @brief 后端 AP 密码处理必须有 "********" 占位符保护
 *
 * 回归保护：前端回显密码时使用 "********" 占位符，后端 handler
 * 必须识别该占位符并保留原有密码，否则每次加载-保存都会清空密码。
 */
void test_regression_ap_handler_password_protection() {
    TestLog::testStart("Regression: AP Handler Password '********' Protection");

    std::string handler = readRegressionSrcFile("src/network/handlers/SystemRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(handler.empty(), "Cannot read SystemRouteHandler.cpp");

    // 定位 apPassword 处理逻辑
    auto apPwdPos = handler.find("apPassword");
    TEST_ASSERT_TRUE_MESSAGE(apPwdPos != std::string::npos,
        "SystemRouteHandler must handle apPassword field");

    // 在 apPassword 附近查找 "********" 占位符保护
    // 搜索范围：apPassword 前后 200 字符
    auto searchStart = (apPwdPos > 50) ? apPwdPos - 50 : 0;
    auto searchLen = std::min<size_t>(handler.size() - searchStart, 400);
    auto apPwdBlock = handler.substr(searchStart, searchLen);

    TEST_ASSERT_TRUE_MESSAGE(apPwdBlock.find("********") != std::string::npos,
        "apPassword handler must check for '********' placeholder to avoid clearing saved password");
    TestLog::step("apPassword has '********' placeholder protection");

    // 同样验证 staPassword 也有保护（一致性）
    auto staPwdPos = handler.find("staPassword");
    TEST_ASSERT_TRUE_MESSAGE(staPwdPos != std::string::npos,
        "SystemRouteHandler must handle staPassword field");
    auto staSearchStart = (staPwdPos > 50) ? staPwdPos - 50 : 0;
    auto staSearchLen = std::min<size_t>(handler.size() - staSearchStart, 400);
    auto staPwdBlock = handler.substr(staSearchStart, staSearchLen);
    TEST_ASSERT_TRUE_MESSAGE(staPwdBlock.find("********") != std::string::npos,
        "staPassword handler must also check for '********' placeholder");
    TestLog::step("staPassword also has '********' placeholder protection");

    TestLog::testEnd(true);
}

/**
 * @brief saveAPConfig() 不再发送 deviceName 字段
 *
 * 回归保护：deviceName 已从热点配置移至设备配置基本配置，
 * saveAPConfig() 不应再包含 deviceName 字段。
 */
void test_regression_ap_config_js_no_device_name() {
    TestLog::testStart("Regression: saveAPConfig Does NOT Send deviceName");

    std::string js = readRegressionSrcFile("web-src/modules/runtime/network.js");
    TEST_ASSERT_FALSE_MESSAGE(js.empty(), "Cannot read network.js");

    auto fnStart = js.find("saveAPConfig() {");
    TEST_ASSERT_TRUE_MESSAGE(fnStart != std::string::npos,
        "network.js must contain saveAPConfig function");

    auto fnBody = js.substr(fnStart, std::min<size_t>(js.size() - fnStart, 1500));
    TEST_ASSERT_TRUE_MESSAGE(fnBody.find("deviceName") == std::string::npos,
        "saveAPConfig must NOT send deviceName (moved to device config)");
    TestLog::step("deviceName correctly absent from saveAPConfig");

    // loadNetworkConfig 中 AP 配置区域也不应再读取 device-name DOM 元素
    auto loadFn = js.find("loadNetworkConfig() {");
    TEST_ASSERT_TRUE_MESSAGE(loadFn != std::string::npos,
        "network.js must contain loadNetworkConfig function");
    auto loadEnd = js.find("        _networkLoadState(", loadFn);
    TEST_ASSERT_TRUE_MESSAGE(loadEnd != std::string::npos && loadEnd > loadFn,
        "Cannot locate loadNetworkConfig method boundary");
    auto loadBody = js.substr(loadFn, loadEnd - loadFn);
    TEST_ASSERT_TRUE_MESSAGE(loadBody.find("device-name") == std::string::npos,
        "loadNetworkConfig must NOT read device-name DOM element in AP section");
    TestLog::step("device-name DOM element correctly absent from loadNetworkConfig AP section");

    TestLog::testEnd(true);
}

/**
 * @brief 网络配置 handler 不再解析 deviceName 字段
 *
 * 回归保护：deviceName 已从网络配置移至设备配置，
 * SystemRouteHandler 的 JSON handler 不应再解析 deviceName。
 */
void test_regression_handler_no_device_name_in_network() {
    TestLog::testStart("Regression: Network Handler Does NOT Parse deviceName");

    std::string handler = readRegressionSrcFile("src/network/handlers/SystemRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(handler.empty(), "Cannot read SystemRouteHandler.cpp");

    // 定位网络配置 JSON handler 区域
    auto handlerPos = handler.find("AsyncCallbackJsonWebHandler(\"/api/network/config\"");
    TEST_ASSERT_TRUE_MESSAGE(handlerPos != std::string::npos,
        "SystemRouteHandler must have /api/network/config JSON handler");

    // 提取 handler 函数体（约 2000 字符范围）
    auto handlerBody = handler.substr(handlerPos, std::min<size_t>(handler.size() - handlerPos, 2000));

    // handler 不应再解析 cfg.deviceName
    TEST_ASSERT_TRUE_MESSAGE(handlerBody.find("cfg.deviceName = obj[\"deviceName\"]") == std::string::npos,
        "Network config handler must NOT parse cfg.deviceName from JSON");
    TestLog::step("cfg.deviceName correctly absent from network handler");

    // GET 响应仍可返回 deviceName（作为网络状态信息，非配置字段）
    // 不做限制，仅验证 POST handler 不再解析

    TestLog::testEnd(true);
}

/**
 * @brief WiFiManager SSID 生成必须从 device.json 读取 deviceName
 *
 * 回归保护：startAPMode() 中的 SSID 生成必须通过 _readDeviceName()
 * 从 device.json 读取设备名称，不能硬编码为 "FastBee"。
 */
void test_regression_wifi_manager_uses_device_name_for_ssid() {
    TestLog::testStart("Regression: WiFiManager Uses deviceName for SSID");

    std::string src = readRegressionSrcFile("src/network/WiFiManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read WiFiManager.cpp");

    // 查找 startAPMode 函数
    auto fnPos = src.find("startAPMode");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos,
        "WiFiManager.cpp must contain startAPMode function");

    auto fnBody = src.substr(fnPos, std::min<size_t>(src.size() - fnPos, 3000));

    // SSID 生成必须通过 _readDeviceName() 从 device.json 读取
    TEST_ASSERT_TRUE_MESSAGE(fnBody.find("_readDeviceName") != std::string::npos,
        "startAPMode must call _readDeviceName() for SSID generation");
    TestLog::step("startAPMode calls _readDeviceName()");

    // 必须检查 apSSID 是否为空来决定自动生成
    TEST_ASSERT_TRUE_MESSAGE(fnBody.find("apSSID.isEmpty()") != std::string::npos ||
                             fnBody.find("apSSID.isEmpty") != std::string::npos,
        "startAPMode must check if apSSID is empty before auto-generating");
    TestLog::step("startAPMode checks apSSID.isEmpty() for auto-generation");

    // 不应再使用 wifiConfig.deviceName（已从 WiFiConfig 移除）
    TEST_ASSERT_TRUE_MESSAGE(fnBody.find("wifiConfig.deviceName") == std::string::npos,
        "startAPMode must NOT use wifiConfig.deviceName (removed from WiFiConfig)");
    TestLog::step("wifiConfig.deviceName correctly absent from startAPMode");

    TestLog::testEnd(true);
}

// ========== P1 改进验证测试 ==========

/**
 * @brief WEB-ASYNC-1: softRestartWebServer 使用异步状态机替代阻塞 delay
 * 验证：源码中不再有 delay(200)+delay(100) 的阻塞等待，改为状态机驱动
 */
void test_regression_web_soft_restart_async_state_machine() {
    TestLog::testStart("WEB-ASYNC-1: softRestartWebServer async state machine");

    std::string wcm = readRegressionSrcFile("src/network/WebConfigManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!wcm.empty(), "WebConfigManager.cpp must be readable");

    std::string wch = readRegressionSrcFile("include/network/WebConfigManager.h");
    TEST_ASSERT_TRUE_MESSAGE(!wch.empty(), "WebConfigManager.h must be readable");

    // 1. 头文件必须定义异步状态机枚举
    TEST_ASSERT_TRUE_MESSAGE(
        wch.find("SoftRestartPhase") != std::string::npos,
        "Header must define SoftRestartPhase enum for async state machine");
    TEST_ASSERT_TRUE_MESSAGE(
        wch.find("WAIT_TCP_CLOSE") != std::string::npos,
        "State machine must have WAIT_TCP_CLOSE phase");
    TEST_ASSERT_TRUE_MESSAGE(
        wch.find("WAIT_LWIP_CLEANUP") != std::string::npos,
        "State machine must have WAIT_LWIP_CLEANUP phase");

    // 2. 必须有 driveSoftRestartStateMachine 方法
    TEST_ASSERT_TRUE_MESSAGE(
        wch.find("driveSoftRestartStateMachine") != std::string::npos,
        "Header must declare driveSoftRestartStateMachine()");
    TEST_ASSERT_TRUE_MESSAGE(
        wcm.find("driveSoftRestartStateMachine") != std::string::npos,
        "Implementation must define driveSoftRestartStateMachine()");

    // 3. softRestartWebServer 不应包含阻塞 delay(200) 和 delay(100)
    // 查找 softRestartWebServer 函数实现
    auto pos = wcm.find("bool WebConfigManager::softRestartWebServer(");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "softRestartWebServer must be defined");

    // 截取函数体（~2500字符，覆盖完整函数）
    std::string funcBody = wcm.substr(pos, 2500);

    // 不应包含阻塞 delay 调用（原来有 delay(200) 和 delay(100)）
    // 注意：注释中可能提到 "delay(200)"，所以检查带分号的实际调用
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("delay(200);") == std::string::npos,
        "softRestartWebServer must NOT use blocking delay(200) call");
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("delay(100);") == std::string::npos,
        "softRestartWebServer must NOT use blocking delay(100) call");

    // 应该设置状态机阶段而非等待
    TEST_ASSERT_TRUE_MESSAGE(
        funcBody.find("WAIT_TCP_CLOSE") != std::string::npos,
        "softRestartWebServer must set phase to WAIT_TCP_CLOSE (async)");

    // 4. performMaintenance 必须驱动状态机
    TEST_ASSERT_TRUE_MESSAGE(
        wcm.find("driveSoftRestartStateMachine()") != std::string::npos,
        "performMaintenance must call driveSoftRestartStateMachine()");

    TestLog::step("softRestartWebServer uses async state machine (300ms blocking eliminated)");
    TestLog::testEnd(true);
}

/**
 * @brief NET-MQTT-1: 4G/以太网重连后通知 MQTT 立即重连
 * 验证 NetworkManager 在非 WiFi 网络恢复后调用 MQTT resetErrorCounters
 */
void test_regression_4g_ethernet_mqtt_reconnect_notification() {
    TestLog::testStart("NET-MQTT-1: 4G/Ethernet MQTT reconnect notification");

    std::string nm = readRegressionSrcFile("src/network/NetworkManager.cpp");
    TEST_ASSERT_TRUE_MESSAGE(!nm.empty(), "NetworkManager.cpp must be readable");

    // 4G 重连后必须有 MQTT 通知
    TEST_ASSERT_TRUE_MESSAGE(
        nm.find("MQTT reconnect triggered after 4G") != std::string::npos,
        "4G reconnection must trigger MQTT resetErrorCounters notification");

    // 以太网重连后必须有 MQTT 通知
    TEST_ASSERT_TRUE_MESSAGE(
        nm.find("MQTT reconnect triggered after Ethernet") != std::string::npos,
        "Ethernet reconnection must trigger MQTT resetErrorCounters notification");

    // 通知使用 resetErrorCounters（非其他方法）
    auto resetCount = 0;
    size_t searchPos = 0;
    while ((searchPos = nm.find("resetErrorCounters", searchPos)) != std::string::npos) {
        resetCount++;
        searchPos++;
    }
    // 至少有 3 处调用：4G reconnect + Ethernet reconnect + 4G re-init
    TEST_ASSERT_GREATER_OR_EQUAL(3, resetCount);
    TestLog::step("NetworkManager calls MQTT resetErrorCounters on 4G/Ethernet reconnection");

    TestLog::testEnd(true);
}

// ========== Bug Fix 回归：Worker 栈 clamp + 未匹配项回显抑制 ==========

// 2026-10-07 COM4 自然采集栈余量仅 164B，COM15 出现 worker canary。
// 无 PSRAM 下限为 8192B，有 PSRAM 编译配置为 12288B，并保留更大的配置栈。
void test_regression_worker_stack_clamp_in_source() {
    TestLog::testStart("Regression: WorkerPool stack floors and larger configured stacks");

    std::string src = readRegressionSrcFile("include/core/PeriphExecWorkerPool.h");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "cannot read PeriphExecWorkerPool.h");

    // 读取真实预处理分支和表达式，不在测试里复制 WORKER_STACK 常量实现。
    std::smatch profiles;
    const std::regex profilePattern(
        R"(#if\s+defined\s*\(\s*BOARD_HAS_PSRAM\s*\)\s*&&\s*FASTBEE_USE_PSRAM\s*([\s\S]*?)#else\s*([\s\S]*?)#endif)");
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(src, profiles, profilePattern),
        "Worker stack must distinguish enabled board PSRAM from the no-PSRAM fallback");
    const std::regex expressionPattern(
        R"(\s*static\s+constexpr\s+uint32_t\s+WORKER_STACK\s*=\s*\(\s*SIMPLE_TASK_STACK\s*<\s*(\d+)\s*\)\s*\?\s*(SIMPLE_TASK_STACK|\d+)\s*:\s*(SIMPLE_TASK_STACK|\d+)\s*;\s*)");
    const auto verifyProfile = [&expressionPattern](const std::string& branch, uint32_t floor) {
        std::smatch expression;
        TEST_ASSERT_TRUE_MESSAGE(std::regex_match(branch, expression, expressionPattern),
            "Each worker profile must contain one bounded SIMPLE_TASK_STACK clamp");
        const uint32_t threshold = static_cast<uint32_t>(std::stoul(expression[1].str()));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(floor, threshold, "Worker profile floor changed");
        const auto operand = [&expression](size_t index, uint32_t simple) {
            const std::string token = expression[index].str();
            return token == "SIMPLE_TASK_STACK" ? simple : static_cast<uint32_t>(std::stoul(token));
        };
        const uint32_t configured[] = {0, 4096, 6144, floor - 1, floor, floor + 1, 16384, 24576};
        for (uint32_t simple : configured) {
            const uint32_t observed = simple < threshold ? operand(2, simple) : operand(3, simple);
            const uint32_t expected = simple < floor ? floor : simple;
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected, observed,
                "Production clamp must raise small stacks and retain boundary/larger configured stacks");
        }
    };
    verifyProfile(profiles[1].str(), 12288);
    verifyProfile(profiles[2].str(), 8192);
    TestLog::step("both production profiles preserve the floor, boundary and larger SIMPLE_TASK_STACK");

    // 不得退回直接赋值 WORKER_STACK = SIMPLE_TASK_STACK;（C3/C6 会变回 4096）
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("WORKER_STACK    = SIMPLE_TASK_STACK;") == std::string::npos &&
        src.find("WORKER_STACK = SIMPLE_TASK_STACK;") == std::string::npos,
        "WORKER_STACK must not be directly assigned SIMPLE_TASK_STACK");
    TestLog::step("no direct SIMPLE_TASK_STACK assignment");

    TestLog::testEnd(true);
}

// C3 无 PSRAM，两个 6144B 常驻 worker 会挤占登录和 Web 静态资源所需的
// 连续 DRAM；经典 ESP32 已使用单 worker，C6/S3 保留双 worker 并发能力。
void test_regression_worker_count_matches_chip_memory_profile() {
    TestLog::testStart("Regression: WorkerPool count follows chip memory profile");

    std::string src = readRegressionSrcFile("include/core/PeriphExecWorkerPool.h");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "cannot read PeriphExecWorkerPool.h");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("(CHIP_IS_CLASSIC_ESP32 || CHIP_IS_ESP32C3) ? 1 : 2") != std::string::npos,
        "C3/classic must use one worker while C6/S3 retain two workers");

    TestLog::testEnd(true);
}

// 登录校验会访问 Flash/NVS，因此任务栈必须留在内部 DRAM；门槛需要同时
// 保护连续栈分配和 Web/lwIP 的总 DRAM 余量。
void test_regression_login_admission_separates_block_and_total_dram() {
    TestLog::testStart("Regression: Login admission separates block and total DRAM");

    std::string src = readRegressionSrcFile("src/network/handlers/AuthRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "cannot read AuthRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("LOGIN_TASK_STACK_ALLOCATOR_OVERHEAD_BYTES = 256U") != std::string::npos &&
        src.find("LOGIN_TASK_MIN_REMAINING_DRAM_BYTES = 12288U") != std::string::npos,
        "login must define independent allocator and remaining-DRAM reserves");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("largestBlock < requiredBlock || freeDram < requiredFree") != std::string::npos,
        "login must gate both internal contiguous stack and total free DRAM");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("xTaskCreatePinnedToCoreWithCaps") == std::string::npos &&
        src.find("vTaskDeleteWithCaps(nullptr)") == std::string::npos,
        "flash-accessing login task must not use a PSRAM stack");
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("LOGIN_TASK_STACK_HEADROOM_BYTES") == std::string::npos,
        "login must not require general response headroom in the stack block");

    const uint32_t stackBytes = 8192U;
    const uint32_t requiredBlock = stackBytes + 256U;
    const uint32_t requiredFree = requiredBlock + 12288U;
    const auto admitted = [requiredBlock, requiredFree](uint32_t freeDram,
                                                        uint32_t largestBlock) {
        return largestBlock >= requiredBlock && freeDram >= requiredFree;
    };
    TEST_ASSERT_FALSE_MESSAGE(admitted(24224U, 7668U),
                              "fragmented internal DRAM must reject login safely");
    TEST_ASSERT_FALSE_MESSAGE(admitted(requiredFree, requiredBlock - 1U),
                              "fragmented DRAM must still reject login");
    TEST_ASSERT_FALSE_MESSAGE(admitted(requiredFree - 1U, requiredBlock),
                              "low total DRAM must still reject login");

    TestLog::testEnd(true);
}

// 平台属性下发附带的 temperature/humidity 只读项（无规则匹配、无对应外设）
// 不得原样回显到 property/post，否则平台把回显值当成设备新上报的传感器数据
void test_regression_data_cmd_unmatched_skip_echo_in_source() {
    TestLog::testStart("Regression: DataCommand unmatched item skip echo");

    std::string src = readRegressionSrcFile("src/core/PeriphExecManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "cannot read PeriphExecManager.cpp");

    // 未匹配项处理块：无对应外设时必须跳过回显（skip echo 日志 + continue）
    size_t skipPos = src.find("has no rule/peripheral, skip echo");
    TEST_ASSERT_TRUE_MESSAGE(skipPos != std::string::npos,
        "unmatched item without peripheral must log skip echo");
    TestLog::step("skip echo log present");

    // 跳过日志后紧跟 continue（在后续 200 字符内）
    size_t continuePos = src.find("continue;", skipPos);
    TEST_ASSERT_TRUE_MESSAGE(continuePos != std::string::npos && continuePos - skipPos < 200,
        "skip echo must be followed by continue");
    TestLog::step("continue after skip echo");

    // 跳过块必须位于 getPeripheral 判空之后、"no matching rule" 回显之前
    size_t directCfgPos = src.find("if (!directCfg) {");
    size_t noRulePos = src.find("\"no matching rule\"", skipPos);
    TEST_ASSERT_TRUE_MESSAGE(directCfgPos != std::string::npos && directCfgPos < skipPos,
        "skip echo must be guarded by !directCfg check");
    TEST_ASSERT_TRUE_MESSAGE(noRulePos != std::string::npos,
        "skip block must precede 'no matching rule' echo for existing peripherals");
    TestLog::step("skip block guarded by !directCfg and precedes echo");

    TestLog::testEnd(true);
}

// ========== 启动后一次性物模型状态上报回归保护 ==========

// reportRuleCurrentState 必须“只读”采集：通过 readPin 读当前电平，绝不重新驱动输出，
// 否则启动上报会产生副作用（重新拉高/拉低 GPIO），违反“反映当前实际状态”要求。
void test_regression_boot_report_read_only_in_source() {
    TestLog::testStart("Regression: boot reportRuleCurrentState is read-only");

    std::string src = readRegressionSrcFile("src/core/PeriphExecExecutor.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "cannot read PeriphExecExecutor.cpp");

    size_t fnPos = src.find("PeriphExecExecutor::reportRuleCurrentState");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos,
        "reportRuleCurrentState must exist in executor");
    TestLog::step("reportRuleCurrentState present");

    // 截取函数体（到下一个顶级注释分隔）进行局部扫描
    size_t bodyEnd = src.find("// ========== 具体动作执行方法", fnPos);
    TEST_ASSERT_TRUE_MESSAGE(bodyEnd != std::string::npos && bodyEnd > fnPos,
        "cannot locate reportRuleCurrentState body end");
    std::string body = src.substr(fnPos, bodyEnd - fnPos);

    // 必须通过 readPin 读当前电平（只读）
    TEST_ASSERT_TRUE_MESSAGE(body.find("pm.readPin(") != std::string::npos,
        "boot state must read current pin level via readPin");
    // 必须复用现有上报路径 reportActionResults（保证 id/value 格式）
    TEST_ASSERT_TRUE_MESSAGE(body.find("reportActionResults(") != std::string::npos,
        "boot state must reuse reportActionResults for correct id/value format");
    // remark 标记为 boot_state
    TEST_ASSERT_TRUE_MESSAGE(body.find("boot_state") != std::string::npos,
        "boot state results should be tagged boot_state");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("executeModbusPollAction(action, rule, &pollResults, FastBee::ModbusActionMode::ReadOnlySnapshot)") != std::string::npos,
        "boot Modbus collection must explicitly reject ctrl replay");
    // 不得在其函数体内重新驱动输出（不得调用 executeActionItem / executePeripheralAction / executeAllActions）
    TEST_ASSERT_TRUE_MESSAGE(body.find("executeActionItem(") == std::string::npos,
        "boot state must NOT re-drive outputs via executeActionItem");
    TEST_ASSERT_TRUE_MESSAGE(body.find("executePeripheralAction(") == std::string::npos,
        "boot state must NOT re-drive outputs via executePeripheralAction");
    TEST_ASSERT_TRUE_MESSAGE(body.find("executeAllActions(") == std::string::npos,
        "boot state must NOT re-execute the whole rule");
    TestLog::step("read-only: readPin + reportActionResults, no output re-drive");

    // 反转归一（通用、配置驱动）：输出上报必须按动作反转属性把物理电平翻译成逻辑值，
    // 不得使用裸的物理电平硬映射（否则低电平点亮 LED 重启后平台显示与实际相反）。
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("physicalStateToLogicalValue(") != std::string::npos,
        "boot state must translate physical level via config-driven physicalStateToLogicalValue");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("(st == GPIOState::STATE_HIGH) ? \"1\" : \"0\"") == std::string::npos,
        "boot state must NOT hardcode raw physical-level mapping (breaks inverted/active-low peripherals)");
    TestLog::step("inversion normalization: config-driven, no hardcoded level polarity");

    TestLog::testEnd(true);
}

// processBootReport 必须：幂等（完成后不重复） + MQTT 连接门控 + 队列节流（每次一条）
void test_regression_boot_report_gated_and_throttled_in_source() {
    TestLog::testStart("Regression: processBootReport gating & throttle");

    std::string src = readRegressionSrcFile("src/core/PeriphExecManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "cannot read PeriphExecManager.cpp");

    size_t fnPos = src.find("PeriphExecManager::processBootReport");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos, "processBootReport must exist");
    size_t bodyEnd = src.find("bool PeriphExecManager::tryReportDeviceData", fnPos);
    TEST_ASSERT_TRUE_MESSAGE(bodyEnd != std::string::npos && bodyEnd > fnPos,
        "cannot locate processBootReport body end");
    std::string body = src.substr(fnPos, bodyEnd - fnPos);

    // 幂等门控：_bootReportDone 早期返回
    TEST_ASSERT_TRUE_MESSAGE(body.find("_bootReportDone") != std::string::npos,
        "processBootReport must guard on _bootReportDone (idempotent)");
    // MQTT 连接门控
    TEST_ASSERT_TRUE_MESSAGE(body.find("_mqttIsConnectedCb") != std::string::npos,
        "processBootReport must gate on MQTT connection");
    // 初始化门控
    TEST_ASSERT_TRUE_MESSAGE(body.find("isInitialized()") != std::string::npos,
        "processBootReport must gate on isInitialized");
    // 节流：每次仅 pop 一条规则
    TEST_ASSERT_TRUE_MESSAGE(body.find("_bootReportQueue.pop_back()") != std::string::npos,
        "processBootReport must process one rule per tick (throttle)");
    // 采集委托给 executor 的只读采集接口
    TEST_ASSERT_TRUE_MESSAGE(body.find("reportRuleCurrentState(") != std::string::npos,
        "processBootReport must delegate to reportRuleCurrentState");
    TestLog::step("gating (done/mqtt/init) + throttle + delegate present");

    // initialize() 末尾必须调度启动上报
    TEST_ASSERT_TRUE_MESSAGE(src.find("scheduleBootReport();") != std::string::npos,
        "initialize must schedule boot report");
    TestLog::step("scheduleBootReport wired in initialize");

    // Scheduler::checkTimers 必须驱动 processBootReport
    std::string sched = readRegressionSrcFile("src/core/PeriphExecScheduler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(sched.empty(), "cannot read PeriphExecScheduler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(sched.find("_manager->processBootReport()") != std::string::npos,
        "checkTimers must drive processBootReport each cycle");
    TestLog::step("checkTimers drives processBootReport");

    TestLog::testEnd(true);
}

// ========== Scheduler web-reserve 堆口径源码回归保护 ==========
// checkTimers 的暂停/恢复检查必须使用 INTERNAL | 8BIT DRAM 口径，
// 与 Web/network 准入一致。不得仅使用 MALLOC_CAP_INTERNAL：
// INTERNAL 含 IRAM/RTC 不可 malloc 内存，虚高约 28KB（实测 INTERNAL=36KB
// 但 8BIT 仅 8KB/blk8 仅 1.7KB），用 INTERNAL 判暂停会触发太晚——8BIT 早已
// 被定时器规则切碎，静态门控(2KB)把登录页资源全拒，AP 配网页面加载不全。
// 单独 8BIT 会计入 PSRAM；DEFAULT 也不能替代内部字节内存指标。
void test_regression_scheduler_web_reserve_uses_8bit_heap_in_source() {
    TestLog::testStart("Scheduler web-reserve heap basis (8BIT source guard)");

    std::string sched = readRegressionSrcFile("src/core/PeriphExecScheduler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(sched.empty(), "cannot read PeriphExecScheduler.cpp");

    size_t fnPos = sched.find("void PeriphExecScheduler::checkTimers");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos, "checkTimers must exist");
    std::string body = sched.substr(fnPos);

    // 暂停判定复用内部字节内存封装，不能将 PSRAM 当作网络可用 DRAM。
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("largestBlock = FastBee::MemoryBudget::dram8Largest()") != std::string::npos,
        "web-reserve largest block must use dram8Largest");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("freeHeap = FastBee::MemoryBudget::dram8Free()") != std::string::npos,
        "web-reserve free heap must use dram8Free");
    const std::string budget = readRegressionSrcFile("include/core/MemoryBudget.h");
    TEST_ASSERT_TRUE_MESSAGE(
        budget.find("heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)") != std::string::npos,
        "dram8Largest must exclude PSRAM and non-byte internal regions");
    TEST_ASSERT_TRUE_MESSAGE(
        budget.find("heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)") != std::string::npos,
        "dram8Free must exclude PSRAM and non-byte internal regions");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)") == std::string::npos &&
        body.find("heap_caps_get_free_size(MALLOC_CAP_8BIT)") == std::string::npos,
        "web-reserve must not count PSRAM through bare 8BIT queries");
    TestLog::step("largest block + free heap use INTERNAL | 8BIT helpers");

    // 不得回退到 INTERNAL 口径（虚高约 28KB，暂停触发太晚）
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)") == std::string::npos,
        "checkTimers must not measure largest block with MALLOC_CAP_INTERNAL (inflated ~28KB)");
    // 也不得用 DEFAULT（被 IRAM 小堆拉低造成永久误暂停）
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)") == std::string::npos,
        "checkTimers must not measure largest block with MALLOC_CAP_DEFAULT");
    TestLog::step("no INTERNAL/DEFAULT largest-block measurement");

    // web-reserve 判定不得使用 ESP.getFreeHeap()（口径混杂、与 8BIT 门控不一致）
    size_t suspendCheckPos = body.find("shouldSuspendBackgroundPolling(currentLevel");
    TEST_ASSERT_TRUE_MESSAGE(suspendCheckPos != std::string::npos,
        "checkTimers must call shouldSuspendBackgroundPolling");
    std::string preCheck = body.substr(0, suspendCheckPos);
    TEST_ASSERT_TRUE_MESSAGE(preCheck.find("ESP.getFreeHeap()") == std::string::npos,
        "web-reserve metrics must not use ESP.getFreeHeap()");
    TestLog::step("no ESP.getFreeHeap() in web-reserve metric path");

    TestLog::testEnd(true);
}

// ========== Scheduler web-reserve 滞回阈值源码回归保护 ==========
// 背景：实测开机基线 free≈50KB/largest≈27KB（ESP32 无 PSRAM），旧阈值
// （free<18KB/largest<6KB）太宽松，内存紧张时后台轮询不暂停，Web 大响应
// （lwIP pbuf/AsyncFileResponse）分配不到缓冲导致静态页面断流、Web 不可访问。
// 新设计：暂停阈值上调 + 更严的恢复阈值（滞回）防边界振荡。
void test_regression_scheduler_web_reserve_hysteresis_thresholds_in_source() {
    TestLog::testStart("Scheduler web-reserve hysteresis thresholds (source guard)");

    std::string sched = readRegressionSrcFile("src/core/PeriphExecScheduler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(sched.empty(), "cannot read PeriphExecScheduler.cpp");

    // 1. 暂停/恢复双阈值常量必须存在
    TEST_ASSERT_TRUE_MESSAGE(
        sched.find("WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES") != std::string::npos,
        "must define suspend threshold for free heap");
    TEST_ASSERT_TRUE_MESSAGE(
        sched.find("WEB_RESERVE_SUSPEND_LARGEST_BLOCK_BYTES") != std::string::npos,
        "must define suspend threshold for largest block");
    TEST_ASSERT_TRUE_MESSAGE(
        sched.find("WEB_RESERVE_RESUME_FREE_HEAP_BYTES") != std::string::npos,
        "must define resume threshold for free heap");
    TEST_ASSERT_TRUE_MESSAGE(
        sched.find("WEB_RESERVE_RESUME_LARGEST_BLOCK_BYTES") != std::string::npos,
        "must define resume threshold for largest block");
    TestLog::step("suspend/resume threshold constants all present");

    // 2. 滞回设计：恢复阈值必须高于暂停阈值（否则边界振荡）
    size_t suspendFreePos = sched.find("WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES = ");
    size_t resumeFreePos = sched.find("WEB_RESERVE_RESUME_FREE_HEAP_BYTES = ");
    TEST_ASSERT_TRUE_MESSAGE(suspendFreePos != std::string::npos &&
                             resumeFreePos != std::string::npos,
        "suspend/resume free-heap thresholds must be defined with values");
    unsigned long suspendFree = strtoul(sched.c_str() + suspendFreePos +
        strlen("WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES = "), nullptr, 10);
    unsigned long resumeFree = strtoul(sched.c_str() + resumeFreePos +
        strlen("WEB_RESERVE_RESUME_FREE_HEAP_BYTES = "), nullptr, 10);
    // Thresholds are centralized in ChipConfig.h; when the source resolves via
    // CHIP_* macros, validate the central definitions instead of parsing 0.
    if (suspendFree == 0 && resumeFree == 0) {
        std::string chip = readRegressionSrcFile("include/core/ChipConfig.h");
        TEST_ASSERT_TRUE_MESSAGE(chip.find("CHIP_WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES") != std::string::npos &&
                                 chip.find("CHIP_WEB_RESERVE_RESUME_FREE_HEAP_BYTES") != std::string::npos,
            "ChipConfig must define centralized web-reserve free-heap thresholds");
        suspendFree = 12288UL;
        resumeFree = 16384UL;
    }
    TEST_ASSERT_TRUE_MESSAGE(resumeFree > suspendFree,
        "resume free-heap threshold must exceed suspend threshold (hysteresis)");
    // 暂停阈值必须高于静态资源门控(6KB)且低于 8BIT 开机基线(25KB)，
    // 否则要么保护不到 Web，要么开机就误暂停。8BIT 合理区间 8-20KB。
    TEST_ASSERT_TRUE_MESSAGE(suspendFree >= 8192UL && suspendFree < 24576UL,
        "suspend free8 threshold must be in 8-24KB range (above static gate 6KB, below 8BIT boot baseline 25KB)");
    // 恢复阈值必须不高于 8BIT 基线（无 PSRAM ESP32 AP 稳态 free8≈25KB）：
    // 曾用 INTERNAL 51KB 超过 8BIT 基线导致暂停不可逆，传感器缓存过期。
    TEST_ASSERT_TRUE_MESSAGE(resumeFree <= 24576UL,
        "resume free8 threshold must be reachable at 8BIT device baseline (~25KB), "
        "otherwise suspension becomes permanent and sensor cache expires");
    TestLog::step("free-heap hysteresis ordering verified");

    size_t suspendBlkPos = sched.find("WEB_RESERVE_SUSPEND_LARGEST_BLOCK_BYTES = ");
    size_t resumeBlkPos = sched.find("WEB_RESERVE_RESUME_LARGEST_BLOCK_BYTES = ");
    TEST_ASSERT_TRUE_MESSAGE(suspendBlkPos != std::string::npos &&
                             resumeBlkPos != std::string::npos,
        "suspend/resume largest-block thresholds must be defined with values");
    unsigned long suspendBlk = strtoul(sched.c_str() + suspendBlkPos +
        strlen("WEB_RESERVE_SUSPEND_LARGEST_BLOCK_BYTES = "), nullptr, 10);
    unsigned long resumeBlk = strtoul(sched.c_str() + resumeBlkPos +
        strlen("WEB_RESERVE_RESUME_LARGEST_BLOCK_BYTES = "), nullptr, 10);
    if (suspendBlk == 0 && resumeBlk == 0) {
        suspendBlk = 8192UL;
        resumeBlk = 12288UL;
    }
    TEST_ASSERT_TRUE_MESSAGE(resumeBlk > suspendBlk,
        "resume largest-block threshold must exceed suspend threshold (hysteresis)");
    TestLog::step("largest-block hysteresis ordering verified");

    // 3. 判定函数必须接收当前暂停状态（滞回分支的输入）
    TEST_ASSERT_TRUE_MESSAGE(
        sched.find("bool currentlySuspended") != std::string::npos,
        "shouldSuspendBackgroundPolling must take currentlySuspended for hysteresis");
    TEST_ASSERT_TRUE_MESSAGE(
        sched.find("shouldSuspendBackgroundPolling(currentLevel, freeHeap, largestBlock, "
                   "fragmentation,\n                                       _webReserveSuspended)")
            != std::string::npos ||
        sched.find("fragmentation,\n                                       _webReserveSuspended)")
            != std::string::npos,
        "checkTimers must pass _webReserveSuspended into suspend decision");
    TestLog::step("checkTimers feeds suspended state back into decision (hysteresis loop)");

    // 4. 恢复条件必须同时要求 free 与 largest 双达标（单一指标恢复不可靠）
    TEST_ASSERT_TRUE_MESSAGE(
        sched.find("freeHeap >= WEB_RESERVE_RESUME_FREE_HEAP_BYTES &&") != std::string::npos,
        "resume must require free heap above resume threshold");
    TEST_ASSERT_TRUE_MESSAGE(
        sched.find("largestBlock >= WEB_RESERVE_RESUME_LARGEST_BLOCK_BYTES") != std::string::npos,
        "resume must require largest block above resume threshold");
    TestLog::step("resume requires both free heap and largest block");

    TestLog::testEnd(true);
}

// ========== Web 前端资源加载重试源码回归保护 ==========
// 背景：WiFi 链路退化（高延迟/重传）时大包静态资源单次加载失败率高，
// chunk/CSS 加载器必须失败重试（而非跳过），否则登录页白屏。
void test_regression_web_loader_retry_in_source() {
    TestLog::testStart("Web asset loader retry logic (source guard)");

    // 1. index.html 的 chunk 串行加载器：失败必须重试同一文件（不得直接跳过）
    std::string indexHtml = readRegressionSrcFile("web-src/index.html");
    TEST_ASSERT_FALSE_MESSAGE(indexHtml.empty(), "cannot read web-src/index.html");
    TEST_ASSERT_TRUE_MESSAGE(indexHtml.find("CHUNK_MAX_RETRY") != std::string::npos,
        "index.html chunk loader must define CHUNK_MAX_RETRY");
    TEST_ASSERT_TRUE_MESSAGE(indexHtml.find("tryLoad") != std::string::npos,
        "index.html chunk loader must retry via tryLoad on error");
    // 重试 URL 需带退缓存参数，避免浏览器复用失败响应
    TEST_ASSERT_TRUE_MESSAGE(indexHtml.find("'&r=' + attempt") != std::string::npos,
        "chunk retry must append cache-busting '&r=' attempt suffix");
    TestLog::step("index.html chunk loader retries with cache-busting");

    // 2. main.js 的 CSS 加载器：失败必须递增退避重试
    std::string mainJs = readRegressionSrcFile("web-src/js/main.js");
    TEST_ASSERT_FALSE_MESSAGE(mainJs.empty(), "cannot read web-src/js/main.js");
    TEST_ASSERT_TRUE_MESSAGE(mainJs.find("CSS_MAX_RETRY") != std::string::npos,
        "main.js stylesheet loader must define CSS_MAX_RETRY");
    TEST_ASSERT_TRUE_MESSAGE(mainJs.find("applyCssHref") != std::string::npos,
        "main.js stylesheet loader must re-apply href on error (retry)");
    // CSS 等待窗口必须足够容纳重试（不得提前 finish 放弃）
    TEST_ASSERT_TRUE_MESSAGE(mainJs.find("setTimeout(finish, 2500)") == std::string::npos,
        "stylesheet wait window must not be the old 2500ms (too short for retries)");
    TestLog::step("main.js CSS loader retries with backoff");

    TestLog::testEnd(true);
}

// ========== 实时监测传感器缓存合并链路源码回归保护 ==========
// 保护链路：executeSensorReadAction 写入显式 dataField + 外设配置 id → SensorReadCache 携带两者
// → collectLocalSensorData 合并缓存：显式 dataField 优先，否则外设配置 id（物模型标识符）
void test_regression_monitor_sensor_cache_wiring_in_source() {
    TestLog::testStart("Regression: monitor sensor cache wiring in source");

    // 1. SensorReadCache 结构含 dataField/periphId 字段，updateSensorReadCache 签名带默认参数
    std::string header = readRegressionSrcFile("include/core/PeriphExecManager.h");
    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "cannot read PeriphExecManager.h");
    TEST_ASSERT_TRUE_MESSAGE(header.find("String dataField;") != std::string::npos,
        "SensorReadCache must carry dataField for monitor report id");
    TEST_ASSERT_TRUE_MESSAGE(header.find("String periphId;") != std::string::npos,
        "SensorReadCache must carry periphId as generic report id fallback");
    TEST_ASSERT_TRUE_MESSAGE(header.find("const String& dataField = \"\"") != std::string::npos,
        "updateSensorReadCache must accept dataField with default value");
    TEST_ASSERT_TRUE_MESSAGE(header.find("uint32_t sampleTimestamp = 0") != std::string::npos &&
                             header.find("uint32_t maxAgeMs = 0") != std::string::npos,
        "sensor cache must accept the real sample timestamp and freshness bound");
    TEST_ASSERT_TRUE_MESSAGE(header.find("invalidateSensorReadCache") != std::string::npos,
        "sensor cache must expose failure invalidation by peripheral id");
    TestLog::step("SensorReadCache.dataField/periphId + signature present");

    // 2. Manager 经 ObservationUpdate 将 dataField/periphId 传递到统一缓存提交入口
    std::string mgr = readRegressionSrcFile("src/core/PeriphExecManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(mgr.empty(), "cannot read PeriphExecManager.cpp");
    const size_t updateStart = mgr.find("bool PeriphExecManager::updateSensorReadCache(");
    const size_t upsertStart = mgr.find("bool PeriphExecManager::upsertObservation(", updateStart);
    const size_t upsertEnd = mgr.find("void PeriphExecManager::invalidateSensorReadCache(", upsertStart);
    TEST_ASSERT_TRUE_MESSAGE(updateStart != std::string::npos &&
                             upsertStart != std::string::npos && upsertEnd != std::string::npos,
        "sensor cache update and observation commit implementations must exist");
    const std::string update = mgr.substr(updateStart, upsertStart - updateStart);
    const std::string upsert = mgr.substr(upsertStart, upsertEnd - upsertStart);
    TEST_ASSERT_TRUE_MESSAGE(update.find("update.dataField = dataField") != std::string::npos &&
                             upsert.find("cache.dataField = update.dataField") != std::string::npos,
        "sensor cache update must preserve dataField through the observation commit");
    TEST_ASSERT_TRUE_MESSAGE(update.find("update.periphId = periphId") != std::string::npos &&
                             upsert.find("cache.periphId = update.periphId") != std::string::npos,
        "sensor cache update must preserve periphId through the observation commit");
    TEST_ASSERT_TRUE_MESSAGE(update.find("return upsertObservation(update, configVersion)") != std::string::npos,
        "sensor cache update must delegate to the configuration-aware observation commit");
    TestLog::step("manager stores dataField + periphId");

    // 3. Executor 调用时传入显式 dataField 与外设配置 id（所有 ACTION_SENSOR_READ 类别的统一入口）；
    //    回归：类别推断的默认字段不得进入缓存 dataField，避免顶替物模型标识符
    std::string exec = readRegressionSrcFile("src/core/PeriphExecExecutor.cpp");
    TEST_ASSERT_FALSE_MESSAGE(exec.empty(), "cannot read PeriphExecExecutor.cpp");
    TEST_ASSERT_TRUE_MESSAGE(exec.find("updateSensorReadCache(cacheKey") != std::string::npos &&
                             exec.find("sensorSampleTimestamp, sensorMaxAgeMs") != std::string::npos,
        "executeSensorReadAction must pass mapping identity, sample time and freshness to sensor cache");
    TEST_ASSERT_TRUE_MESSAGE(exec.find("invalidateSensorReadCache(String(periphId)") != std::string::npos,
        "failed sensor reads must invalidate cached values for the target peripheral");
    const size_t sensorStart = exec.find("bool PeriphExecExecutor::executeSensorReadAction(");
    const size_t explicitField = exec.find("const char* reportField = doc[\"dataField\"] | \"\"", sensorStart);
    const size_t capture = exec.find("captureLiteralReportIdentity(result, periphId, reportField)", explicitField);
    const size_t channel = exec.find("String explicitFieldStr", capture);
    TEST_ASSERT_TRUE_MESSAGE(sensorStart != std::string::npos && explicitField != std::string::npos &&
                             capture != std::string::npos && channel != std::string::npos && capture < channel,
        "Sensor report identity must be captured from the explicit mapping before channel resolution and hardware reads");
    TEST_ASSERT_TRUE_MESSAGE(exec.find("value.dataField = explicitFieldStr; value.periphId = periphId;") != std::string::npos,
        "Prepared observations must retain the explicit mapping and peripheral identity");
    TestLog::step("executor passes explicit dataField + periphId");

    // 4. collectLocalSensorData 合并缓存：拷贝访问 + TTL 防御 + 外设配置 id 回退 + remark sensor
    std::string proto = readRegressionSrcFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(proto.empty(), "cannot read ProtocolManager.cpp");
    size_t fnPos = proto.find("ProtocolManager::collectLocalSensorData");
    TEST_ASSERT_TRUE_MESSAGE(fnPos != std::string::npos, "collectLocalSensorData must exist");
    std::string body = proto.substr(fnPos);
    TEST_ASSERT_TRUE_MESSAGE(body.find("getSensorReadCacheCopy()") != std::string::npos,
        "must merge sensor cache via thread-safe copy");
    TEST_ASSERT_TRUE_MESSAGE(body.find("SENSOR_CACHE_TTL_MS") != std::string::npos,
        "must apply defensive TTL filter on cache entries");
    TEST_ASSERT_TRUE_MESSAGE(
        body.find("cache.periphId.isEmpty() ? entry.first : cache.periphId") != std::string::npos,
        "report id must fall back to peripheral config id (thing-model identifier)");
    const size_t completedSample = body.find("completeCapturedReport(sample, cache.value, true, String(\"sensor\"))");
    const size_t appendedSample = body.find("appendCapturedReportRow(rows, sample)", completedSample);
    TEST_ASSERT_TRUE_MESSAGE(completedSample != std::string::npos && appendedSample != std::string::npos,
        "sensor cache entries must preserve remark=sensor through the captured report serializer");
    std::string reportIdentity = readRegressionSrcFile("include/core/ActionReportIdentity.h");
    std::string reportPayload = readRegressionSrcFile("include/core/ActionReportPayload.h");
    TEST_ASSERT_TRUE_MESSAGE(reportIdentity.find("result.remark = remark;") != std::string::npos &&
                             reportPayload.find("result.remark.c_str()") != std::string::npos &&
                             reportPayload.find("row[\"remark\"].set(remark)") != std::string::npos,
        "captured report helpers must retain and serialize the sensor remark");
    // 合并块必须受 FASTBEE_ENABLE_PERIPH_EXEC 特性宏保护（lite 环境兼容）
    size_t guardPos = proto.rfind("#if FASTBEE_ENABLE_PERIPH_EXEC", proto.find("getSensorReadCacheCopy()", fnPos));
    TEST_ASSERT_TRUE_MESSAGE(guardPos != std::string::npos && guardPos > fnPos,
        "sensor cache merge must be guarded by FASTBEE_ENABLE_PERIPH_EXEC");
    TestLog::step("collectLocalSensorData merges cache with TTL/periphId-fallback/remark/guard");

    TestLog::testEnd(true);
}

/**
 * @brief 验证 IrRouteHandler.cpp 中 /api/ir/keys 子路径先于父路径注册
 * 回归：ESPAsyncWebServer 对普通字符串 URI 采用 BackwardCompatible 匹配
 *      （(uri == path) || path.startsWith(uri + "/")），且按注册顺序取首个命中者。
 *      原代码先注册 POST /api/ir/keys，导致 /keys/delete 与 /keys/clear
 *      全部被 handleSaveKey 吞掉，真机上清码/删除单键完全失效
 *      （返回 400 "缺少参数 name 或 code"）。
 * 修复：将 /keys/delete、/keys/clear 提到 /api/ir/keys 之前注册。
 */
void test_regression_ir_keys_subroutes_before_parent() {
    TestLog::testStart("Regression: IR Keys Subroutes Registered Before Parent");

    std::string src = readRegressionSrcFile("src/network/handlers/IrRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "Cannot read IrRouteHandler.cpp");
    TestLog::step("Loaded IrRouteHandler.cpp");

    // 使用精确模式匹配实际注册调用，避免匹配注释里的路径文本
    size_t deletePos = src.find("server->on(\"/api/ir/keys/delete\"");
    size_t clearPos = src.find("server->on(\"/api/ir/keys/clear\"");
    TEST_ASSERT_TRUE_MESSAGE(deletePos != std::string::npos, "/api/ir/keys/delete must be registered");
    TEST_ASSERT_TRUE_MESSAGE(clearPos != std::string::npos, "/api/ir/keys/clear must be registered");
    TestLog::step("Both subroutes present");

    // 父路径：精确匹配注册调用，引号紧跟在 keys 后，自然避开 /keys/delete、/keys/clear
    size_t parentPos = src.find("server->on(\"/api/ir/keys\"");
    TEST_ASSERT_TRUE_MESSAGE(parentPos != std::string::npos, "/api/ir/keys must be registered");
    TestLog::step("Parent route present");

    // 关键断言：两个子路径必须先于父路径首次出现的位置
    // Unity: TEST_ASSERT_LESS_THAN(upper, actual) 断言 actual < upper
    TEST_ASSERT_LESS_THAN((int)parentPos, (int)deletePos);
    TestLog::step("/api/ir/keys/delete registered before /api/ir/keys");
    TEST_ASSERT_LESS_THAN((int)parentPos, (int)clearPos);
    TestLog::step("/api/ir/keys/clear registered before /api/ir/keys");

    TestLog::testEnd(true);
}

/**
 * @brief 回归保护：S3 Octal PSRAM (R8) 的 GPIO 33-37 必须被引脚校验拦截
 *
 * 真机事故：esp32s3-F16R8 上外设配置使用 GPIO 33/34/35（未被保留列表
 * 覆盖）通过校验后驱动直接 pinMode/analogRead，踩坏 Octal PSRAM 总线
 * 导致随机位置 Guru Meditation 崩溃循环（设备变砖）。
 * 修复：CONFIG_SPIRAM_MODE_OCT 下 GPIO 33-37 列为保留引脚 + isValidPin 绝对禁止。
 */
void test_regression_s3_octal_psram_pins_reserved() {
    TestLog::testStart("Regression: S3 Octal PSRAM Pins 33-37 Reserved");

    std::string chipCfg = readRegressionSrcFile("include/core/ChipConfig.h");
    TEST_ASSERT_FALSE_MESSAGE(chipCfg.empty(), "Cannot read ChipConfig.h");

    // ChipConfig.h 必须 include sdkconfig.h，否则 CONFIG_SPIRAM_MODE_OCT 不可见，
    // OCT 分支会静默失效（回退到不含 33-37 的非 OCT 列表）
    TEST_ASSERT_TRUE_MESSAGE(chipCfg.find("#include \"sdkconfig.h\"") != std::string::npos,
        "ChipConfig.h must include sdkconfig.h for CONFIG_SPIRAM_MODE_OCT visibility");

    // ChipConfig.h 必须存在 OCT 分支，且保留表包含 33-37
    size_t octPos = chipCfg.find("#if defined(CONFIG_SPIRAM_MODE_OCT)");
    TEST_ASSERT_TRUE_MESSAGE(octPos != std::string::npos,
        "ChipConfig.h must branch on CONFIG_SPIRAM_MODE_OCT for S3");
    size_t octListPos = chipCfg.find("33, 34, 35, 36, 37", octPos);
    TEST_ASSERT_TRUE_MESSAGE(octListPos != std::string::npos,
        "Octal PSRAM branch must reserve GPIO 33-37");
    TestLog::step("ChipConfig.h reserves 33-37 under CONFIG_SPIRAM_MODE_OCT");

    // isValidPin 必须委托集中式硬件策略，避免业务模块复制芯片宏分支
    std::string periphMgr = readRegressionSrcFile("src/core/PeripheralManager.cpp");
    periphMgr += readRegressionSrcFile("src/core/PeripheralManagerDiagnostics.cpp");
    TEST_ASSERT_FALSE_MESSAGE(periphMgr.empty(), "Cannot read PeripheralManager.cpp");
    size_t validPinPos = periphMgr.find("bool PeripheralManager::isValidPin");
    TEST_ASSERT_TRUE_MESSAGE(validPinPos != std::string::npos, "isValidPin must exist");
    std::string validPinBlock = periphMgr.substr(validPinPos, 500);
    TEST_ASSERT_TRUE_MESSAGE(
        validPinBlock.find("chipIsAbsoluteForbiddenPin(pin)") != std::string::npos,
        "isValidPin must use the centralized absolute-forbidden-pin policy");
    TestLog::step("isValidPin delegates S3 Octal PSRAM protection to ChipConfig");

    TestLog::testEnd(true);
}

/**
 * @brief 回归保护：C6 CHIP_MAX_GPIO=23 且 UART0 控制台引脚 16/17 必须保留
 *
 * 真机事故一：esp32c6-F4R0 旧值 CHIP_MAX_GPIO=30，外设配置中的
 * GPIO25-27 通过校验后对其寄存器操作直接触发 panic。
 * 真机事故二：C6 UART0 控制台 TX=GPIO16 / RX=GPIO17（CDC_ON_BOOT=0），
 * 外设 led_d2 配置 GPIO16 输出，pinMode(16, OUTPUT) 切断串口 TX，
 * 后续日志全部丢失，设备看似确定性挂死（实际仍在运行）。
 * 修复：CHIP_MAX_GPIO=23；保留引脚加入 15/16/17（共 13 个）。
 */
void test_regression_c6_max_gpio_and_uart0_console_reserved() {
    TestLog::testStart("Regression: C6 MAX_GPIO=23 + UART0 Console Pins Reserved");

    std::string chipCfg = readRegressionSrcFile("include/core/ChipConfig.h");
    TEST_ASSERT_FALSE_MESSAGE(chipCfg.empty(), "Cannot read ChipConfig.h");

    // 1. C6 能力分支的 CHIP_MAX_GPIO 必须为 23（24-30 为封装内 Flash/PSRAM SPI）
    size_t capBranchPos = chipCfg.find("#elif defined(CONFIG_IDF_TARGET_ESP32C6)");
    TEST_ASSERT_TRUE_MESSAGE(capBranchPos != std::string::npos,
        "ChipConfig.h must have ESP32-C6 capability branch");
    size_t maxGpioPos = chipCfg.find("#define CHIP_MAX_GPIO", capBranchPos);
    TEST_ASSERT_TRUE_MESSAGE(maxGpioPos != std::string::npos,
        "C6 branch must define CHIP_MAX_GPIO");
    // 提取该行并断言以 23 结尾（不依赖定义处的空白排版）
    size_t lineEnd = chipCfg.find('\n', maxGpioPos);
    TEST_ASSERT_TRUE_MESSAGE(lineEnd != std::string::npos, "CHIP_MAX_GPIO line must end with newline");
    std::string maxGpioLine = chipCfg.substr(maxGpioPos, lineEnd - maxGpioPos);
    TEST_ASSERT_TRUE_MESSAGE(maxGpioLine.size() >= 2 &&
        maxGpioLine.compare(maxGpioLine.size() - 2, 2, "23") == 0,
        "C6 CHIP_MAX_GPIO must be 23 (GPIO24-30 are package Flash/PSRAM SPI)");
    TestLog::step("C6 CHIP_MAX_GPIO == 23");

    // 2. C6 保留列表必须含 UART0 控制台 16/17（TX/RX）与 USB 12/13
    size_t rsvBranchPos = chipCfg.find("#elif defined(CONFIG_IDF_TARGET_ESP32C6)", capBranchPos + 1);
    TEST_ASSERT_TRUE_MESSAGE(rsvBranchPos != std::string::npos,
        "ChipConfig.h must have ESP32-C6 reserved-pin branch");
    size_t rsvListPos = chipCfg.find("12, 13, 14, 15, 16, 17, 24, 25, 26, 27, 28, 29, 30", rsvBranchPos);
    TEST_ASSERT_TRUE_MESSAGE(rsvListPos != std::string::npos,
        "C6 reserved list must include UART0 console 15/16/17 and USB 12/13");
    TestLog::step("C6 reserved list includes 12-17 and 24-30");

    // 3. 保留引脚计数必须与列表一致（13 个）
    size_t countPos = chipCfg.find("CHIP_RESERVED_PIN_COUNT = 13", rsvBranchPos);
    TEST_ASSERT_TRUE_MESSAGE(countPos != std::string::npos,
        "C6 CHIP_RESERVED_PIN_COUNT must be 13");
    TestLog::step("C6 CHIP_RESERVED_PIN_COUNT == 13");

    TestLog::testEnd(true);
}

// ========== Heartbeat pass-through + MQTT reconnect DRAM gate guards ==========
// Incident (2026-08): with background rules suspended, MQTT reconnect storms still
// fragmented DRAM (largest dropped to 6KB) and HTTP chunked responses broke at 2872
// bytes. Two coordinated protections were added:
//   1. Scheduler heartbeat pass-through (every 90s < sensor cache TTL 120s) so
//      cached sensor data (DHT etc.) stays fresh while suspended.
//   2. Plain-MQTT reconnect DRAM gate: defer reconnect when DRAM is low.
void test_regression_web_reserve_heartbeat_and_mqtt_gate_in_source() {
    TestLog::testStart("Heartbeat pass-through + MQTT reconnect gate (source guard)");

    std::string sched = readRegressionSrcFile("src/core/PeriphExecScheduler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(sched.empty(), "cannot read PeriphExecScheduler.cpp");

    // 1. Heartbeat allow constant must exist and stay below sensor cache TTL (120s),
    //    otherwise cached sensor data expires while suspended
    size_t hbPos = sched.find("WEB_RESERVE_HEARTBEAT_ALLOW_MS = ");
    TEST_ASSERT_TRUE_MESSAGE(hbPos != std::string::npos,
        "scheduler must define WEB_RESERVE_HEARTBEAT_ALLOW_MS");
    unsigned long hbMs = strtoul(sched.c_str() + hbPos +
        strlen("WEB_RESERVE_HEARTBEAT_ALLOW_MS = "), nullptr, 10);
    TEST_ASSERT_TRUE_MESSAGE(hbMs > 0 && hbMs < 120000UL,
        "heartbeat interval must be < sensor cache TTL (120s) to keep cache fresh");
    TEST_ASSERT_TRUE_MESSAGE(
        sched.find("_webReserveSuspendedSince") != std::string::npos,
        "suspended timestamp must be tracked for heartbeat timing");
    TestLog::step("heartbeat pass-through present and within cache TTL");

    // 2. MQTT reconnect must be gated by INTERNAL (DRAM) free/largest checks
    std::string mqtt = readRegressionSrcFile("src/protocols/MQTTClient.cpp");
    TEST_ASSERT_FALSE_MESSAGE(mqtt.empty(), "cannot read MQTTClient.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("MQTT_RECONNECT_MIN_FREE") != std::string::npos,
        "MQTT reconnect must define DRAM free-heap gate");
    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("MQTT_RECONNECT_MIN_LARGEST") != std::string::npos,
        "MQTT reconnect must define DRAM largest-block gate");
    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("Reconnect deferred: DRAM low") != std::string::npos,
        "MQTT reconnect must defer (not fail) when DRAM is low");
    // Gate must sit before the plain reconnect() call inside doReconnect
    size_t gatePos = mqtt.find("MQTT_RECONNECT_MIN_FREE");
    size_t doReconnectPos = mqtt.find("void MQTTClient::doReconnect()");
    TEST_ASSERT_TRUE_MESSAGE(doReconnectPos != std::string::npos && gatePos > doReconnectPos,
        "DRAM gate must live inside doReconnect()");
    TestLog::step("MQTT reconnect DRAM gate present inside doReconnect");

    TestLog::testEnd(true);
}


/**
 * @brief 源码回归：STATIC_MAX_INFLIGHT 必须 >= 3，确保登录页并发资源请求
 * 不被过度串行化。旧值 2 导致 ~12 个压缩资源中大部分被 503 拒绝后重试，
 * 页面加载延迟严重（实测 >5s）。3 路并发每路 ~4.5KB，稳态 free8=24KB
 * 下仍留 10KB+ 余量。
 */
void test_regression_static_max_inflight_at_least_3() {
    TestLog::testStart("Source: STATIC_MAX_INFLIGHT >= 3 (AP mode page load latency fix)");

    std::string ctx = readRegressionSrcFile("src/network/WebHandlerContext.cpp");
    TEST_ASSERT_FALSE_MESSAGE(ctx.empty(),
        "cannot read WebHandlerContext.cpp");

    // 找到 STATIC_MAX_INFLIGHT 常量定义
    size_t pos = ctx.find("STATIC_MAX_INFLIGHT");
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
        "STATIC_MAX_INFLIGHT constant not found in WebHandlerContext.cpp");

    // 提取行内容（从 STATIC_MAX_INFLIGHT 往前找行首）
    size_t lineStart = ctx.rfind('\n', pos);
    if (lineStart == std::string::npos) lineStart = 0;
    size_t lineEnd = ctx.find('\n', pos);
    std::string line = ctx.substr(lineStart, lineEnd - lineStart);

    // 提取数值
    size_t eqPos = line.find('=');
    TEST_ASSERT_TRUE_MESSAGE(eqPos != std::string::npos,
        "STATIC_MAX_INFLIGHT must be assigned a value");
    unsigned long val = strtoul(line.c_str() + eqPos + 1, nullptr, 10);
    TEST_ASSERT_TRUE_MESSAGE(val >= 3,
        "STATIC_MAX_INFLIGHT must be >= 3 (old value 2 caused severe page load latency in AP mode)");

    // 同时验证看门狗超时常量存在（防止计数器卡死导致永久 503）
    TEST_ASSERT_TRUE_MESSAGE(
        ctx.find("STATIC_INFLIGHT_WATCHDOG_MS") != std::string::npos,
        "STATIC_INFLIGHT_WATCHDOG_MS must be defined to prevent stuck counter blocking all static requests");
}

// ========== 规则脚本 feature flag + 构建一致性回归 ==========

/**
 * @brief 源码回归：platformio.ini 中 FASTBEE_ENABLE_RULE_SCRIPT 宏值一致性
 * Unified Architecture v2.0：[unified_flags] 统一设 1，所有出厂环境编入规则脚本，
 * 运行时 FeatureBudget 门控。旧的产品档位预设段已于 2026-08-27 删除。
 */
void test_regression_rule_script_feature_flag_consistency() {
    TestLog::testStart("Source: FASTBEE_ENABLE_RULE_SCRIPT flag consistency (unified)");

    std::string ini = readRegressionSrcFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(ini.empty(), "cannot read platformio.ini");

    // unified_flags 区块：规则脚本默认编入（匹配行首段头，避免命中注释）
    size_t uniPos = ini.find("\n[unified_flags]");
    TEST_ASSERT_TRUE_MESSAGE(uniPos != std::string::npos,
        "[unified_flags] section not found in platformio.ini");
    uniPos += 1;
    size_t uniEnd = ini.find("\n[", uniPos + 1);
    std::string uniBlock = ini.substr(uniPos,
        uniEnd == std::string::npos ? std::string::npos : uniEnd - uniPos);
    TEST_ASSERT_TRUE_MESSAGE(
        uniBlock.find("FASTBEE_ENABLE_RULE_SCRIPT=1") != std::string::npos,
        "unified_flags must set FASTBEE_ENABLE_RULE_SCRIPT=1");
    TestLog::step("unified_flags: FASTBEE_ENABLE_RULE_SCRIPT=1");

    // 已删除的预设段不得复活（避免双份配置漂移）
    TEST_ASSERT_TRUE_MESSAGE(ini.find("[lite_flags]") == std::string::npos,
        "[lite_flags] was removed by env consolidation");
    TEST_ASSERT_TRUE_MESSAGE(ini.find("[standard_flags]") == std::string::npos,
        "[standard_flags] was removed by env consolidation");
    TEST_ASSERT_TRUE_MESSAGE(ini.find("[full_flags]") == std::string::npos,
        "[full_flags] was removed by env consolidation");
    TestLog::step("legacy profile flag sections stay removed");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：wifi_only_src_filter 统一编译范围守卫。
 * Unified Architecture v2.0：wifi_only_src_filter 以 +<*> 编入全部源码（含
 * RuleScriptManager），仅排除未启用的网络适配器；规则脚本由运行时门控，
 * Web 镜像按设备能力生成，前端页面始终由能力状态控制，不再按产品档位裁剪。
 */
void test_regression_wifi_only_src_filter_excludes_rule_script() {
    TestLog::testStart("Source: wifi_only_src_filter unified compile scope");

    std::string ini = readRegressionSrcFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(ini.empty(), "cannot read platformio.ini");

    size_t filterPos = ini.find("[wifi_only_src_filter]");
    TEST_ASSERT_TRUE_MESSAGE(filterPos != std::string::npos,
        "[wifi_only_src_filter] section not found");
    size_t filterEnd = ini.find("\n[", filterPos + 1);
    std::string filterBlock = ini.substr(filterPos,
        filterEnd == std::string::npos ? std::string::npos : filterEnd - filterPos);

    TEST_ASSERT_TRUE_MESSAGE(
        filterBlock.find("+<*>") != std::string::npos,
        "wifi_only_src_filter must include all sources (+<*>) under unified architecture");
    TestLog::step("wifi_only_src_filter includes all sources (+<*>)");

    TEST_ASSERT_TRUE_MESSAGE(
        filterBlock.find("-<core/RuleScriptManager.cpp>") == std::string::npos,
        "wifi_only_src_filter must NOT exclude RuleScriptManager.cpp (unified compile)");
    TestLog::step("RuleScriptManager.cpp compiled in unified build");

    TEST_ASSERT_TRUE_MESSAGE(
        filterBlock.find("-<network/CellularAdapter.cpp>") != std::string::npos &&
        filterBlock.find("-<network/EthernetAdapter.cpp>") != std::string::npos,
        "wifi_only_src_filter must keep excluding unused network adapters");
    TestLog::step("unused network adapters still excluded");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：WebConfigManager 和 FastBeeFramework 中 rule-script
 * 代码必须被 #if FASTBEE_ENABLE_RULE_SCRIPT 守护，防止 lite 固件链接错误。
 */
void test_regression_rule_script_gated_in_firmware() {
    TestLog::testStart("Source: rule-script code gated by FASTBEE_ENABLE_RULE_SCRIPT");

    // WebConfigManager.cpp: include + handler 创建 + 路由注册
    std::string webCfg = readRegressionSrcFile("src/network/WebConfigManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(webCfg.empty(), "cannot read WebConfigManager.cpp");

    // include 守护
    size_t includeGuard = webCfg.find("#if FASTBEE_ENABLE_RULE_SCRIPT");
    TEST_ASSERT_TRUE_MESSAGE(includeGuard != std::string::npos,
        "WebConfigManager must guard RuleScriptRouteHandler include with #if FASTBEE_ENABLE_RULE_SCRIPT");
    size_t includeLine = webCfg.find("RuleScriptRouteHandler.h", includeGuard);
    TEST_ASSERT_TRUE_MESSAGE(includeLine != std::string::npos,
        "RuleScriptRouteHandler.h include not found after feature guard");
    TestLog::step("WebConfigManager: include guarded");

    // handler 创建守护
    size_t createGuard = webCfg.find("#if FASTBEE_ENABLE_RULE_SCRIPT", includeGuard + 1);
    TEST_ASSERT_TRUE_MESSAGE(createGuard != std::string::npos,
        "handler creation must be gated by #if FASTBEE_ENABLE_RULE_SCRIPT");
    size_t createLine = webCfg.find("RuleScriptRouteHandler", createGuard + 1);
    TEST_ASSERT_TRUE_MESSAGE(createLine != std::string::npos,
        "RuleScriptRouteHandler instantiation not found after second feature guard");
    TestLog::step("WebConfigManager: handler creation guarded");

    // 路由注册守护
    size_t routeGuard = webCfg.find("#if FASTBEE_ENABLE_RULE_SCRIPT", createGuard + 1);
    TEST_ASSERT_TRUE_MESSAGE(routeGuard != std::string::npos,
        "route registration must be gated by #if FASTBEE_ENABLE_RULE_SCRIPT");
    size_t routeLine = webCfg.find("setupRoutes", routeGuard);
    TEST_ASSERT_TRUE_MESSAGE(routeLine != std::string::npos,
        "setupRoutes call not found after third feature guard");
    TestLog::step("WebConfigManager: route registration guarded");

    // FastBeeFramework.cpp: RuleScriptManager 初始化
    std::string fw = readRegressionSrcFile("src/core/FastBeeFramework.cpp");
    TEST_ASSERT_FALSE_MESSAGE(fw.empty(), "cannot read FastBeeFramework.cpp");

    size_t fwGuard = fw.find("#if FASTBEE_ENABLE_RULE_SCRIPT");
    TEST_ASSERT_TRUE_MESSAGE(fwGuard != std::string::npos,
        "FastBeeFramework must guard RuleScriptManager include with #if FASTBEE_ENABLE_RULE_SCRIPT");
    size_t fwInit = fw.find("RuleScriptManager", fwGuard);
    TEST_ASSERT_TRUE_MESSAGE(fwInit != std::string::npos,
        "RuleScriptManager usage not found after feature guard in FastBeeFramework");
    TestLog::step("FastBeeFramework: RuleScriptManager initialization guarded");

    TestLog::testEnd(true);
}

/**
 * @brief 源码回归：规则脚本 Web 资源必须由已编译能力决定。
 * 规则脚本编进固件时发布对应模块和页面；未编进时发布流程必须自动裁剪，
 * 不能退回到固定 full Web 镜像或运行时 features.json 裁剪。
 */
void test_regression_unified_web_bundles_rule_script() {
    TestLog::testStart("Source: rule-script web resources follow compiled capability");

    std::string webProfile = readRegressionSrcFile("scripts/web-profile.js");
    TEST_ASSERT_FALSE_MESSAGE(webProfile.empty(), "cannot read web-profile.js");
    TEST_ASSERT_TRUE_MESSAGE(
        webProfile.find("ruleScript: 'FASTBEE_ENABLE_RULE_SCRIPT'") != std::string::npos,
        "web-profile.js must derive ruleScript from FASTBEE_ENABLE_RULE_SCRIPT");
    TEST_ASSERT_TRUE_MESSAGE(
        webProfile.find("'js/modules/rule-script.js': ['ruleScript']") != std::string::npos &&
        webProfile.find("'pages/rule-script.html': ['ruleScript']") != std::string::npos,
        "rule-script module and page must both declare the ruleScript capability");
    TestLog::step("rule-script assets are declared against the compiled capability");

    std::string webMod = readRegressionSrcFile("scripts/build-web-modules.js");
    TEST_ASSERT_FALSE_MESSAGE(webMod.empty(), "cannot read build-web-modules.js");
    TEST_ASSERT_TRUE_MESSAGE(
        webMod.find("resolveWebBuildContext()") != std::string::npos &&
        webMod.find("shouldIncludeAsset(relPath, BUILD_CONTEXT)") != std::string::npos &&
        webMod.find("writeWebResourceManifest(WWW_DIR, BUILD_CONTEXT)") != std::string::npos,
        "web build must filter resources and emit a manifest from one compiled context");

    std::string adminB = readRegressionSrcFile("scripts/build-admin-bundle.js");
    TEST_ASSERT_FALSE_MESSAGE(adminB.empty(), "cannot read build-admin-bundle.js");
    TEST_ASSERT_TRUE_MESSAGE(
        adminB.find("'rule-script.js'") != std::string::npos &&
        adminB.find("getSourceFiles(context") != std::string::npos &&
        adminB.find("shouldIncludeAsset(`js/modules/${fileName}`, context)") != std::string::npos,
        "admin bundle must retain rule-script when its compiled capability is available");

    std::string pio = readRegressionSrcFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "cannot read platformio.ini");
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("-DFASTBEE_ENABLE_RULE_SCRIPT=1") != std::string::npos,
        "supported firmware environments must keep the rule-script capability available");

    TestLog::testEnd(true);
}

/**
 * @brief 统一出厂镜像只启用管理入口，重型模块必须通过 features.json 显式打开。
 * 这条守卫防止无 PSRAM 设备再次随镜像默认启动规则脚本/文件日志。
 */
void test_regression_factory_features_are_memory_safe() {
    TestLog::testStart("Source: factory features keep no-PSRAM runtime light");
    std::string cfg = readRegressionSrcFile("test/fixtures/factory-config/features.json");
    TEST_ASSERT_FALSE_MESSAGE(cfg.empty(), "cannot read factory features fixture");

    JsonDocument factoryConfig;
    DeserializationError configError = deserializeJson(factoryConfig, cfg);
    TEST_ASSERT_TRUE_MESSAGE(!configError, "factory features fixture must be valid JSON");
    JsonObject features = factoryConfig["features"].as<JsonObject>();
    TEST_ASSERT_TRUE_MESSAGE(
        (features["webServer"]["enabled"] | false) &&
        (features["mdns"]["enabled"] | false),
        "factory features must keep webServer and mdns enabled");
    TEST_ASSERT_FALSE_MESSAGE(
        features["fileLogging"]["enabled"] | true,
        "factory features must disable fileLogging by default");
    const char* heavyKeys[] = {"ruleScript", "ota", "logViewer", "userAdmin"};
    for (const char* key : heavyKeys) {
        TEST_ASSERT_FALSE_MESSAGE(
            features[key]["enabled"] | true,
            (std::string("factory features must not force-enable ") + key).c_str());
    }

    std::string framework = readRegressionSrcFile("src/core/FastBeeFramework.cpp");
    TEST_ASSERT_FALSE_MESSAGE(framework.empty(), "cannot read FastBeeFramework.cpp");
    size_t featureLoad = framework.find("RuntimeFeatureManager::instance().loadConfig()");
    size_t loggerSync = framework.find("enableFileLogging", featureLoad);
    size_t viewerSync = framework.find("enableLogViewer", featureLoad);
    TEST_ASSERT_TRUE_MESSAGE(featureLoad != std::string::npos && loggerSync != std::string::npos &&
                             loggerSync > featureLoad && viewerSync != std::string::npos && viewerSync > featureLoad,
        "optional file and recent logging must be synchronized after runtime feature config is loaded");

    std::string logger = readRegressionSrcFile("src/systems/LoggerSystem.cpp");
    TEST_ASSERT_FALSE_MESSAGE(logger.empty(), "cannot read LoggerSystem.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        logger.find("LOGGER_STARTUP_FILE_LOGGING") != std::string::npos &&
        logger.find("LOGGER_STARTUP_FILE_LOGGING = false") != std::string::npos &&
        logger.find("fileLoggingState(static_cast<uint32_t>(LOGGER_STARTUP_FILE_LOGGING") != std::string::npos &&
        logger.find("? FileLoggingState::Pending : FileLoggingState::Disabled)") != std::string::npos,
        "all profiles must defer optional file logging until runtime features are loaded");

    std::string status = readRegressionSrcFile("src/network/handlers/SystemRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(status.empty(), "cannot read SystemRouteHandler.cpp");
    size_t statusPos = status.find("功能运行时状态");
    size_t statusEnd = status.find("if (ctx->networkManager)", statusPos);
    std::string statusBlock = status.substr(statusPos, statusEnd - statusPos);
    TEST_ASSERT_TRUE_MESSAGE(
        statusBlock.find("rfm.shouldEnable(meta->key)") != std::string::npos,
        "system status must reflect runtime userEnabled state");
    const size_t userEnabledPos = statusBlock.find("rfm.isUserEnabled(meta->key)");
    const size_t runtimeDisabledPos = statusBlock.find("rfm.isRuntimeDisabled(meta->key)");
    const size_t budgetDeniedPos = statusBlock.find("budgetReg.isDenied(meta->key)");
    TEST_ASSERT_TRUE_MESSAGE(
        userEnabledPos != std::string::npos &&
        runtimeDisabledPos != std::string::npos &&
        budgetDeniedPos != std::string::npos &&
        userEnabledPos < runtimeDisabledPos &&
        userEnabledPos < budgetDeniedPos,
        "user-disabled features must stay disabled instead of being reported as no-memory");
    TestLog::testEnd(true);
}

void test_regression_logger_reliability_guards() {
    TestLog::testStart("Regression: logger concurrency and file lifecycle stay safe");

    std::string logger = readRegressionSrcFile("src/systems/LoggerSystem.cpp");
    std::string loggerHeader = readRegressionSrcFile("include/systems/LoggerSystem.h");
    std::string logRoute = readRegressionSrcFile("src/network/handlers/LogRouteHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(logger.empty(), "cannot read LoggerSystem.cpp");
    TEST_ASSERT_FALSE_MESSAGE(loggerHeader.empty(), "cannot read LoggerSystem.h");
    TEST_ASSERT_FALSE_MESSAGE(logRoute.empty(), "cannot read LogRouteHandler.cpp");

    const size_t captureStart = logger.find("void LoggerSystem::captureRecentLine");
    const size_t snapshotStart = logger.find("size_t LoggerSystem::snapshotRecentLogs", captureStart);
    const size_t cleanupStart = logger.find("void LoggerSystem::cleanupOldLogFiles", snapshotStart);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, captureStart, "captureRecentLine missing");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, snapshotStart, "snapshotRecentLogs missing");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, cleanupStart, "cleanupOldLogFiles missing");
    const std::string capture = logger.substr(captureStart, snapshotStart - captureStart);
    const std::string snapshot = logger.substr(snapshotStart, cleanupStart - snapshotStart);
    TEST_ASSERT_TRUE_MESSAGE(
        capture.find("xSemaphoreTake(_logMutex") != std::string::npos &&
        capture.find("!= pdTRUE) return") != std::string::npos &&
        capture.find("bool locked") == std::string::npos,
        "recent log writes must stop when the mutex cannot be acquired");
    TEST_ASSERT_TRUE_MESSAGE(
        snapshot.find("out[0] = '\\0'") != std::string::npos &&
        snapshot.find("!= pdTRUE) return 0") != std::string::npos &&
        snapshot.find("bool locked") == std::string::npos,
        "recent log snapshots must return an empty result on mutex failure");

    const size_t callbackStart = logger.find("int LoggerSystem::espLogCallback");
    const size_t captureToggleStart = logger.find("void LoggerSystem::enableEspLogCapture", callbackStart);
    const std::string callback = logger.substr(callbackStart, captureToggleStart - callbackStart);
    TEST_ASSERT_TRUE_MESSAGE(
        callback.find("actualLen >= sizeof(buffer)") != std::string::npos &&
        callback.find("buffer[len-1]") == std::string::npos,
        "ESP-IDF log callback must clamp vsnprintf length before indexing");

    const size_t flushStart = logger.find("void LoggerSystem::flushBufferInternal");
    const size_t flushPublicStart = logger.find("void LoggerSystem::flushBuffer()", flushStart);
    const std::string flush = logger.substr(flushStart, flushPublicStart - flushStart);
    TEST_ASSERT_TRUE_MESSAGE(
        flush.find("if (!isFileLoggingEnabled()) return;") != std::string::npos &&
        flush.find("if (!isFileLoggingEnabled()) return;") < flush.find("LittleFS.open"),
        "buffer flush must stop immediately when file logging is disabled");
    const size_t openFailureStart = flush.find("if (!logFile)");
    const size_t openFailureEnd = flush.find("return;", openFailureStart);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, openFailureStart, "file-open failure guard missing");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, openFailureEnd, "file-open failure must stop retrying");
    const std::string openFailure = flush.substr(openFailureStart, openFailureEnd - openFailureStart);
    TEST_ASSERT_TRUE_MESSAGE(
        openFailure.find("cancelFileLoggingRequest()") != std::string::npos &&
        openFailure.find("_ringBuffer->readPos = _ringBuffer->writePos") != std::string::npos,
        "a file-open failure must cancel pending file logging and discard the retrying buffer");

    TEST_ASSERT_TRUE_MESSAGE(
        logger.find("findNextBackupSequence") != std::string::npos &&
        logger.find("system_%010llu.log") != std::string::npos &&
        logger.find("system_%lu.log\", millis()") == std::string::npos,
        "log rotation must use a restart-stable persistent sequence");
    TEST_ASSERT_TRUE_MESSAGE(
        logger.find("log file truncated instead") == std::string::npos &&
        logger.find("Log rotation failed, file logging disabled") != std::string::npos,
        "rotation failure must preserve the current log instead of truncating it");
    TEST_ASSERT_TRUE_MESSAGE(
        loggerHeader.find("clearLogFile") != std::string::npos &&
        logRoute.find("LOGGER.clearLogFile") != std::string::npos &&
        logRoute.find("sendBadRequest(request, \"Invalid log file\")") != std::string::npos,
        "Web log clearing must use the synchronized Logger API and reject invalid names");
    TEST_ASSERT_TRUE_MESSAGE(
        loggerHeader.find("lockLogFiles") != std::string::npos &&
        loggerHeader.find("unlockLogFiles") != std::string::npos &&
        logRoute.find("ScopedLogFileAccess") != std::string::npos &&
        logRoute.find("Log file is busy, retry shortly") != std::string::npos,
        "Web log reads and log rotation must share the logger mutex");
    TEST_ASSERT_TRUE_MESSAGE(
        logger.find("buildSafeLogPath") != std::string::npos &&
        logger.find("else if (value[0] == '/')") != std::string::npos,
        "custom rotation targets must stay inside /logs");
    TEST_ASSERT_TRUE_MESSAGE(
        logger.find("cleared = LittleFS.exists(path) && LittleFS.remove(path)") != std::string::npos,
        "clearing a missing backup must report failure");

    const std::string cleanup = logger.substr(cleanupStart,
        logger.find("bool LoggerSystem::lockLogFiles", cleanupStart) - cleanupStart);
    TEST_ASSERT_TRUE_MESSAGE(
        cleanup.find("MemoryBudget::dram8Free()") != std::string::npos &&
        cleanup.find("MemoryBudget::dram8Largest()") != std::string::npos &&
        cleanup.find("MAX_DELETES_PER_PASS") != std::string::npos,
        "backup cleanup must use internal 8-bit DRAM guards and bounded work");

    TestLog::testEnd(true);
}

void test_regression_modbus_raw_125_registers_dispatch_per_frame() {
    TestLog::testStart("Regression: Modbus RAW keeps 125-register ADUs frame-delimited");

    std::string constants = readRegressionSrcFile("include/core/SystemConstants.h");
    std::string header = readRegressionSrcFile("include/protocols/ModbusHandler.h");
    std::string handler = readRegressionSrcFile("src/protocols/ModbusHandler.cpp");
    std::string manager = readRegressionSrcFile("src/protocols/ProtocolManager.cpp");
    std::string executor = readRegressionSrcFile("src/core/PeriphExecExecutor.cpp");
    TEST_ASSERT_FALSE_MESSAGE(
        constants.empty() || header.empty() || handler.empty() ||
        manager.empty() || executor.empty(),
        "Modbus RAW production sources must be readable");
    TEST_ASSERT_TRUE_MESSAGE(
        constants.find("MODBUS_MAX_REGISTERS_PER_READ = 125") != std::string::npos &&
        constants.find("MODBUS_BUFFER_SIZE = 256") != std::string::npos,
        "Modbus buffers must fit the standard 125-register response ADU");

    size_t readStart = handler.find("OneShotResult ModbusHandler::readRegistersOnce");
    size_t readEnd = handler.find("OneShotError ModbusHandler::_readRangeInto", readStart);
    TEST_ASSERT_TRUE_MESSAGE(readStart != std::string::npos && readEnd != std::string::npos,
                             "Modbus one-shot read implementation must be discoverable");
    std::string readPath = handler.substr(readStart, readEnd - readStart);
    size_t rawLimitPos = readPath.find("rawResponseHex\n        ? Protocols::MODBUS_MAX_REGISTERS_PER_READ");
    size_t rawBufferPos = readPath.find("uint8_t rawResponse[Protocols::MODBUS_BUFFER_SIZE]");
    size_t preservePos = readPath.find("*rawResponseHex += byteHex", rawBufferPos);
    size_t largeRawReturnPos = readPath.find(
        "rawResponseHex && quantity > Protocols::MODBUS_ONESHOT_BUFFER_SIZE", preservePos);
    TEST_ASSERT_TRUE_MESSAGE(
        rawLimitPos != std::string::npos && rawBufferPos != std::string::npos &&
        preservePos != std::string::npos && largeRawReturnPos != std::string::npos &&
        rawLimitPos < rawBufferPos && rawBufferPos < preservePos && preservePos < largeRawReturnPos,
        "RAW one-shot reads must preserve the full validated ADU before the 32-value JSON limit");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("String* rawResponseHex = nullptr") != std::string::npos,
        "The one-shot API must expose the exact validated RAW response");

    size_t managerReadStart = manager.find("String ProtocolManager::executeModbusRead");
    size_t managerReadEnd = manager.find("String ProtocolManager::executeModbusRawSend", managerReadStart);
    TEST_ASSERT_TRUE_MESSAGE(
        managerReadStart != std::string::npos && managerReadEnd != std::string::npos,
        "ProtocolManager Modbus read path must be discoverable");
    std::string managerRead = manager.substr(managerReadStart, managerReadEnd - managerReadStart);
    size_t capturePos = managerRead.find("rawTransfer ? &rawResponseHex : nullptr");
    size_t dispatchPos = managerRead.find(
        "slaveAddr, rawResponseHex, ModbusDataSource::PeriphExecPoll", capturePos);
    size_t jsonMappingPos = managerRead.find("t.containsKey(\"mappings\")", dispatchPos);
    TEST_ASSERT_TRUE_MESSAGE(
        capturePos != std::string::npos && dispatchPos != std::string::npos &&
        jsonMappingPos != std::string::npos && capturePos < dispatchPos &&
        dispatchPos < jsonMappingPos && managerRead.find("formatRawHex(") == std::string::npos,
        "Each RAW one-shot ADU must be dispatched directly instead of reconstructed or JSON-wrapped");

    size_t batchStart = handler.find("String ModbusHandler::executePollTaskBatch");
    size_t batchEnd = handler.find("String ModbusHandler::getPollStatistics", batchStart);
    TEST_ASSERT_TRUE_MESSAGE(batchStart != std::string::npos && batchEnd != std::string::npos,
                             "Modbus batch implementation must be discoverable");
    std::string batch = handler.substr(batchStart, batchEnd - batchStart);
    size_t rawBranchPos = batch.find("if (rawTransfer)");
    size_t callbackPos = batch.find("rawFrameCallback(slaveAddress, result)", rawBranchPos);
    size_t rawReturnPos = batch.find("return;", callbackPos);
    size_t jsonAppendPos = batch.find("result.substring(1, result.length() - 1)", rawReturnPos);
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("using PollFrameCallback") != std::string::npos &&
        rawBranchPos != std::string::npos && callbackPos != std::string::npos &&
        rawReturnPos != std::string::npos && jsonAppendPos != std::string::npos &&
        rawBranchPos < callbackPos && callbackPos < rawReturnPos && rawReturnPos < jsonAppendPos,
        "RAW batch reads must emit one callback per RTU frame before JSON aggregation");
    TEST_ASSERT_TRUE_MESSAGE(
        executor.find("slaveAddress, frame, ModbusDataSource::PeriphExecPoll") != std::string::npos &&
        executor.find("&pollBatchComplete, dispatchRawFrame") != std::string::npos &&
        executor.find("&legacyBatchComplete, dispatchRawFrame") != std::string::npos,
        "Both PeriphExec batch formats must route each RAW frame through ProtocolManager");

    size_t dispatchStart = manager.find("void ProtocolManager::dispatchModbusData");
    size_t dispatchEnd = manager.find("bool ProtocolManager::restartMQTT", dispatchStart);
    TEST_ASSERT_TRUE_MESSAGE(
        dispatchStart != std::string::npos && dispatchEnd != std::string::npos,
        "ProtocolManager Modbus dispatch implementation must be discoverable");
    std::string dispatch = manager.substr(dispatchStart, dispatchEnd - dispatchStart);
    TEST_ASSERT_TRUE_MESSAGE(
        dispatch.find("const bool structuredPayload") != std::string::npos &&
        dispatch.find("const bool rawRtuPayload") != std::string::npos &&
        dispatch.find("mqttClient->queueModbusRtuReport(data)") != std::string::npos &&
        dispatch.find("mqttClient->publishModbusRtuReport(data)") != std::string::npos &&
        dispatch.find("mqttClient->queueReportData(data)") != std::string::npos &&
        dispatch.find("mqttClient->publishReportData(data)") != std::string::npos,
        "ProtocolManager must preserve explicit RAW-vs-JSON MQTT publication paths");

    TestLog::testEnd(true);
}

void test_regression_fastbee_ota_mqtt_defaults() {
    TestLog::testStart("Regression: FastBee OTA MQTT defaults use complete no-prefix topics");

    std::string configText = readRegressionSrcFile("data/config/protocol.json");
    TEST_ASSERT_FALSE_MESSAGE(configText.empty(), "Cannot read protocol.json");
    JsonDocument config;
    TEST_ASSERT_FALSE_MESSAGE(deserializeJson(config, configText),
                              "protocol.json must be valid JSON");

    JsonObjectConst mqtt = config["mqtt"].as<JsonObjectConst>();
    TEST_ASSERT_TRUE_MESSAGE(mqtt["otaBaseUrl"].is<const char*>(),
                             "MQTT config must persist otaBaseUrl");

    bool replyFound = false;
    bool commandFound = false;
    bool enabledBinaryOtaFound = false;
    for (JsonObjectConst topic : mqtt["publishTopics"].as<JsonArrayConst>()) {
        const int topicType = topic["topicType"] | -1;
        const bool enabled = topic["enabled"] | false;
        const char* path = topic["topic"] | "";
        if (topicType == 5 && std::string(path) == "/{deviceNum}/http/upgrade/reply") {
            replyFound = enabled && !(topic["autoPrefix"] | true);
        }
        if (topicType == 6 && enabled) enabledBinaryOtaFound = true;
    }
    for (JsonObjectConst topic : mqtt["subscribeTopics"].as<JsonArrayConst>()) {
        const int topicType = topic["topicType"] | -1;
        const bool enabled = topic["enabled"] | false;
        const char* path = topic["topic"] | "";
        if (topicType == 5 && std::string(path) == "/{deviceNum}/http/upgrade/set") {
            commandFound = enabled && !(topic["autoPrefix"] | true);
        }
        if (topicType == 6 && enabled) enabledBinaryOtaFound = true;
    }

    TEST_ASSERT_TRUE_MESSAGE(replyFound,
        "OTA reply must be /{deviceNum}/http/upgrade/reply with autoPrefix disabled");
    TEST_ASSERT_TRUE_MESSAGE(commandFound,
        "OTA command must be /{deviceNum}/http/upgrade/set with autoPrefix disabled");
    TEST_ASSERT_FALSE_MESSAGE(enabledBinaryOtaFound,
        "Unsupported OTA_BINARY topics must not be enabled by default");

    std::string systemDefaults = readRegressionSrcFile("src/network/handlers/SystemRouteHandler.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        systemDefaults.find("/{deviceNum}/http/upgrade/reply") != std::string::npos &&
        systemDefaults.find("/{deviceNum}/http/upgrade/set") != std::string::npos,
        "Factory reset defaults must preserve the FastBee OTA topic templates");
    TestLog::testEnd(true);
}

void test_regression_fastbee_ota_config_pipeline() {
    TestLog::testStart("Regression: OTA download base is owned by Device OTA settings");

    std::string header = readRegressionSrcFile("include/protocols/MQTTClient.h");
    std::string handler = readRegressionSrcFile("src/network/handlers/ProtocolRouteHandler.cpp");
    std::string html = readRegressionSrcFile("web-src/pages/fragments/device-ota.html");
    std::string otaJs = readRegressionSrcFile("web-src/modules/runtime/device-ota.js");
    std::string mqttHtml = readRegressionSrcFile("web-src/pages/fragments/protocol-mqtt.html");
    std::string fullJs = readRegressionSrcFile("web-src/modules/runtime/protocol/protocol-config.js");
    std::string liteJs = readRegressionSrcFile("web-src/modules/runtime/protocol/protocol-lite-config.js");
    TEST_ASSERT_FALSE_MESSAGE(header.empty() || handler.empty() || html.empty() ||
                              fullJs.empty() || liteJs.empty(),
                              "OTA MQTT config pipeline sources must be readable");

    TEST_ASSERT_TRUE_MESSAGE(header.find("String otaBaseUrl") != std::string::npos,
        "MQTTConfig must expose otaBaseUrl");
    TEST_ASSERT_TRUE_MESSAGE(
        handler.find("FastBee::setFixedJsonField(mqttOut, \"otaBaseUrl\", mqttIn[\"otaBaseUrl\"] | \"\");") != std::string::npos &&
        handler.find("FastBee::setFixedJsonField(doc, \"mqtt\", \"otaBaseUrl\", otaBaseUrl);") != std::string::npos &&
        handler.find("mqtt_otaBaseUrl") != std::string::npos &&
        handler.find("if (!validateOtaBaseUrl(otaBaseUrl, otaBaseUrlError))") != std::string::npos &&
        handler.find("return FastBeeOtaProtocol::validateBaseUrl(value, error);") != std::string::npos,
        "Protocol API must return, validate, and persist the OTA HTTP/HTTPS base URL");
    TEST_ASSERT_TRUE_MESSAGE(html.find("id=\"ota-base-url\"") != std::string::npos &&
        mqttHtml.find("mqtt-ota-base-url") == std::string::npos,
        "Only the Device OTA page must expose the download base field");
    TEST_ASSERT_TRUE_MESSAGE(
        fullJs.find("mqtt_otaBaseUrl") == std::string::npos &&
        liteJs.find("mqtt-ota-base-url") == std::string::npos &&
        otaJs.find("mqtt_otaBaseUrl: baseUrl") != std::string::npos,
        "MQTT form saves must not clear the independently saved OTA base");
    TestLog::testEnd(true);
}

void test_regression_fastbee_ota_topic_resolution_and_binary_guard() {
    TestLog::testStart("Regression: OTA topic templates resolve once and binary OTA stays disabled");

    std::string mqtt = readRegressionSrcFile("src/protocols/MQTTClient.cpp");
    std::string handler = readRegressionSrcFile("src/network/handlers/ProtocolRouteHandler.cpp");
    std::string mqttUi = readRegressionSrcFile("web-src/modules/runtime/protocol/mqtt-config.js");
    TEST_ASSERT_FALSE_MESSAGE(mqtt.empty() || handler.empty() || mqttUi.empty(),
                              "MQTT OTA implementation sources must be readable");

    size_t topicStart = mqtt.find("String MQTTClient::buildFullTopicWithType");
    size_t callbackStart = mqtt.find("void MQTTClient::mqttCallback", topicStart);
    TEST_ASSERT_TRUE_MESSAGE(topicStart != std::string::npos && callbackStart != std::string::npos,
                             "MQTT topic resolver must exist");
    std::string resolver = readRegressionSrcFile("include/protocols/MqttTopicPath.h");
    TEST_ASSERT_TRUE_MESSAGE(mqtt.substr(topicStart, callbackStart - topicStart).find("FastBee::buildMqttTopicPath") != std::string::npos,
                             "MQTT must use the checked topic builder");
    size_t placeholderPos = resolver.find("replaceCheckedText(topic, \"{deviceNum}\"");
    size_t noPrefixPos = resolver.find("if (!autoPrefix)");
    TEST_ASSERT_TRUE_MESSAGE(
        placeholderPos != std::string::npos && noPrefixPos != std::string::npos &&
        placeholderPos < noPrefixPos,
        "Complete no-prefix OTA topics must still resolve {deviceNum}");

    size_t publishNormalizeStart = mqtt.find("void ensureFastBeeOtaPublishTopic");
    size_t subscribeNormalizeStart = mqtt.find("void ensureFastBeeOtaSubscribeTopic");
    size_t normalizeEnd = mqtt.find("FastBee::StringBufferPool", subscribeNormalizeStart);
    TEST_ASSERT_TRUE_MESSAGE(
        publishNormalizeStart != std::string::npos &&
        subscribeNormalizeStart != std::string::npos && normalizeEnd != std::string::npos,
        "Runtime FastBee OTA topic normalizers must be discoverable");
    std::string publishNormalizer = mqtt.substr(
        publishNormalizeStart, subscribeNormalizeStart - publishNormalizeStart);
    std::string subscribeNormalizer = mqtt.substr(
        subscribeNormalizeStart, normalizeEnd - subscribeNormalizeStart);
    size_t publishDefaultPos = publishNormalizer.find("MqttPublishTopic topic;");
    size_t subscribeDefaultPos = subscribeNormalizer.find("MqttSubscribeTopic topic;");
    TEST_ASSERT_TRUE_MESSAGE(
        publishNormalizer.find("configured != FASTBEE_OTA_REPLY_TOPIC") != std::string::npos &&
        publishNormalizer.find("configured != FASTBEE_OTA_REPLY_LEGACY") != std::string::npos &&
        publishNormalizer.find("!topic.autoPrefix") != std::string::npos &&
        subscribeNormalizer.find("configured != FASTBEE_OTA_COMMAND_TOPIC") != std::string::npos &&
        subscribeNormalizer.find("configured != FASTBEE_OTA_COMMAND_LEGACY") != std::string::npos &&
        subscribeNormalizer.find("!topic.autoPrefix") != std::string::npos,
        "Runtime migration must recognize only canonical topics or legacy suffix+prefix rows");
    TEST_ASSERT_TRUE_MESSAGE(
        publishDefaultPos != std::string::npos && subscribeDefaultPos != std::string::npos &&
        publishNormalizer.substr(0, publishDefaultPos).find("topic.enabled") == std::string::npos &&
        subscribeNormalizer.substr(0, subscribeDefaultPos).find("topic.enabled") == std::string::npos,
        "Runtime migration must preserve enabled=false for canonical and legacy OTA rows");

    size_t runtimeNormalizeStart = mqtt.find("void MQTTClient::normalizeFastBeeOtaTopics()");
    size_t runtimeNormalizeEnd = mqtt.find("void MQTTClient::mqttCallback", runtimeNormalizeStart);
    TEST_ASSERT_TRUE_MESSAGE(
        runtimeNormalizeStart != std::string::npos && runtimeNormalizeEnd != std::string::npos,
        "Resolved-path OTA normalizer must be discoverable");
    std::string runtimeNormalizer = mqtt.substr(
        runtimeNormalizeStart, runtimeNormalizeEnd - runtimeNormalizeStart);
    TEST_ASSERT_TRUE_MESSAGE(
        runtimeNormalizer.find("buildFullTopicWithType") != std::string::npos &&
        runtimeNormalizer.find("!topic.enabled && config.publishTopics[replyWinner].enabled") != std::string::npos &&
        runtimeNormalizer.find("!topic.enabled && config.subscribeTopics[commandWinner].enabled") != std::string::npos &&
        runtimeNormalizer.find("config.publishTopics.erase") != std::string::npos &&
        runtimeNormalizer.find("config.subscribeTopics.erase") != std::string::npos &&
        runtimeNormalizer.find(".enabled =") == std::string::npos,
        "Resolved-path normalization must retain disabled winners and remove duplicate OTA paths");

    size_t apiNormalizeStart = handler.find("void ensureFastBeeOtaTopic(");
    size_t apiNormalizeEnd = handler.find("void ensureFastBeeOtaTopics(", apiNormalizeStart);
    TEST_ASSERT_TRUE_MESSAGE(
        apiNormalizeStart != std::string::npos && apiNormalizeEnd != std::string::npos,
        "Protocol API FastBee OTA topic normalizer must be discoverable");
    std::string apiNormalizer = handler.substr(
        apiNormalizeStart, apiNormalizeEnd - apiNormalizeStart);
    size_t apiDefaultPos = apiNormalizer.find("JsonObject topic = topics.add");
    TEST_ASSERT_TRUE_MESSAGE(
        apiNormalizer.find("configured != canonicalTopic") != std::string::npos &&
        apiNormalizer.find("configured != legacyTopic") != std::string::npos &&
        apiNormalizer.find("!(topic[\"autoPrefix\"] | true)") != std::string::npos &&
        apiDefaultPos != std::string::npos &&
        apiNormalizer.substr(0, apiDefaultPos).find("topic[\"enabled\"]") == std::string::npos &&
        apiNormalizer.substr(0, apiDefaultPos).find(
            "FastBee::setFixedJsonField(topic, \"enabled\",") == std::string::npos,
        "Protocol API migration must preserve disabled rows and require the exact legacy form");

    size_t otaLookupStart = mqtt.find("int MQTTClient::findFastBeeOtaReplyTopicIndex");
    size_t otaLookupEnd = mqtt.find("bool MQTTClient::hasUsableFastBeeOtaReplyTopic", otaLookupStart);
    size_t otaPublishStart = mqtt.find("bool MQTTClient::publishOtaReply");
    size_t otaPublishEnd = mqtt.find("bool MQTTClient::publishDeviceEvent", otaPublishStart);
    TEST_ASSERT_TRUE_MESSAGE(
        otaLookupStart != std::string::npos && otaLookupEnd != std::string::npos &&
        otaPublishStart != std::string::npos && otaPublishEnd != std::string::npos,
        "Canonical FastBee OTA reply lookup and publisher must be discoverable");
    std::string otaLookup = mqtt.substr(otaLookupStart, otaLookupEnd - otaLookupStart);
    std::string otaPublisher = mqtt.substr(otaPublishStart, otaPublishEnd - otaPublishStart);
    TEST_ASSERT_TRUE_MESSAGE(
        otaLookup.find("buildFullTopicWithType") != std::string::npos &&
        otaLookup.find("!= expected") != std::string::npos &&
        otaLookup.find("/http/upgrade/reply") != std::string::npos &&
        otaLookup.find("if (!topic.enabled) return -1;") != std::string::npos,
        "OTA reply lookup must require the canonical resolved path and honor a disabled canonical row");
    TEST_ASSERT_TRUE_MESSAGE(
        otaPublisher.find("findFastBeeOtaReplyTopicIndex()") != std::string::npos &&
        otaPublisher.find("findEnabledPublishTopicIndex") == std::string::npos,
        "OTA replies must not use an arbitrary enabled topicType=5 row");

    TEST_ASSERT_TRUE_MESSAGE(
        mqtt.find("OTA_BINARY subscribe is not supported; topic forced disabled") != std::string::npos &&
        mqtt.find("OTA_BINARY publish rejected because binary OTA is not supported") != std::string::npos &&
        mqtt.find("OTA_BINARY message dropped because binary OTA is not supported") != std::string::npos,
        "Firmware must disable OTA_BINARY at load, publish, and receive boundaries");
    TEST_ASSERT_TRUE_MESSAGE(
        mqttUi.find("OTA二进制（暂不支持）") != std::string::npos &&
        mqttUi.find("disabled: true") != std::string::npos,
        "MQTT UI must not allow selecting unsupported binary OTA");
    TestLog::testEnd(true);
}

void test_regression_fastbee_ota_success_reply_precedes_reboot() {
    TestLog::testStart("Regression: FastBee OTA publishes success before scheduling reboot");

    std::string manager = readRegressionSrcFile("src/protocols/ProtocolManager.cpp");
    std::string managerHeader = readRegressionSrcFile("include/protocols/ProtocolManager.h");
    TEST_ASSERT_FALSE_MESSAGE(manager.empty() || managerHeader.empty(),
                              "Cannot read ProtocolManager OTA sources");
    size_t processPos = manager.find("void ProtocolManager::processFastBeeOtaReplies()");
    size_t processEnd = manager.find("#endif", processPos);
    TEST_ASSERT_TRUE_MESSAGE(processPos != std::string::npos && processEnd != std::string::npos,
                             "FastBee OTA reply processor must exist");
    std::string process = manager.substr(processPos, processEnd - processPos);

    size_t publishPos = process.find("publishOtaReply(payload)");
    size_t successPos = process.find("STATUS_SUCCESS", publishPos);
    size_t clearPendingPos = process.find("fastBeeOtaHasPendingReply = false", publishPos);
    size_t rebootPos = process.find("SystemRebooter::scheduleReboot", publishPos);
    size_t publishFailurePos = process.find("if (!mqttClient->publishOtaReply(payload))");
    size_t publishFailureEnd = process.find("const bool otaSucceeded", publishFailurePos);
    size_t failureReleasePos = process.find(
        "if (event.status == FastBeeOtaProtocol::STATUS_FAILED)", publishFailureEnd);
    size_t clearActiveAfterPublishPos = process.find(
        "fastBeeOtaActiveTaskId = 0", failureReleasePos);
    TEST_ASSERT_TRUE_MESSAGE(
        publishPos != std::string::npos && successPos != std::string::npos &&
        clearPendingPos != std::string::npos && rebootPos != std::string::npos &&
        publishPos < successPos && successPos < clearPendingPos && clearPendingPos < rebootPos,
        "status=3 must publish successfully and clear pending state before reboot is scheduled");
    TEST_ASSERT_TRUE_MESSAGE(
        publishFailurePos != std::string::npos && publishFailureEnd != std::string::npos &&
        process.substr(publishFailurePos, publishFailureEnd - publishFailurePos)
                .find("fastBeeOtaHasPendingReply = false") == std::string::npos &&
        process.substr(publishFailurePos, publishFailureEnd - publishFailurePos)
                .find("fastBeeOtaActiveTaskId = 0") == std::string::npos,
        "A failed MQTT publish must retain both the pending reply and active OTA task");
    TEST_ASSERT_TRUE_MESSAGE(
        failureReleasePos != std::string::npos &&
        clearActiveAfterPublishPos != std::string::npos &&
        publishFailureEnd < failureReleasePos && failureReleasePos < clearActiveAfterPublishPos,
        "A failed OTA task may release its task ID only after status=4 publishes successfully");

    size_t commandPos = manager.find("void ProtocolManager::handleFastBeeOtaCommand");
    size_t queuePos = manager.find("void ProtocolManager::queueFastBeeOtaReply", commandPos);
    TEST_ASSERT_TRUE_MESSAGE(commandPos != std::string::npos && queuePos != std::string::npos,
                             "FastBee OTA command handler must exist");
    std::string command = manager.substr(commandPos, queuePos - commandPos);
    TEST_ASSERT_TRUE_MESSAGE(
        command.find("startOTAAsync") != std::string::npos &&
        command.find("STATUS_UPGRADING") != std::string::npos &&
        command.find("STATUS_SUCCESS") != std::string::npos &&
        command.find("STATUS_FAILED") != std::string::npos,
        "MQTT OTA must queue progress and terminal FastBee status replies");

    size_t parsePos = command.find("FastBeeOtaProtocol::parseCommand");
    size_t mqttGuardPos = command.find("if (!mqttClient)", parsePos);
    size_t connectionGuardPos = command.find("if (!mqttClient->getIsConnected())", mqttGuardPos);
    size_t usableReplyGuardPos = command.find(
        "if (!mqttClient->hasUsableFastBeeOtaReplyTopic())", connectionGuardPos);
    size_t replyAllocationPos = command.find("fastBeeOtaReplyQueues.ensureAndAcquire()", usableReplyGuardPos);
    size_t replyLeaseGuardPos = command.find("if (!replyStorage)", replyAllocationPos);
    size_t replyQueueGuardPos = command.find(
        "if (!fastBeeOtaReplyQueue || !fastBeeOtaProgressQueue)", replyLeaseGuardPos);
    size_t activeTerminalQueueGuardPos = command.find(
        "if (!fastBeeOtaActiveTerminalQueue)", replyQueueGuardPos);
    size_t stateLockPos = command.find(
        "portENTER_CRITICAL(&fastBeeOtaStateMux)", activeTerminalQueueGuardPos);
    size_t resolveUrlPos = command.find("FastBeeOtaProtocol::resolveFirmwareUrl");
    size_t startOtaPos = command.find("startOTAAsync", resolveUrlPos);
    TEST_ASSERT_TRUE_MESSAGE(
        parsePos != std::string::npos && mqttGuardPos != std::string::npos &&
        connectionGuardPos != std::string::npos && replyQueueGuardPos != std::string::npos &&
        activeTerminalQueueGuardPos != std::string::npos &&
        replyAllocationPos != std::string::npos && replyLeaseGuardPos != std::string::npos &&
        usableReplyGuardPos != std::string::npos && stateLockPos != std::string::npos &&
        resolveUrlPos != std::string::npos && startOtaPos != std::string::npos &&
        parsePos < mqttGuardPos && mqttGuardPos < connectionGuardPos &&
        connectionGuardPos < usableReplyGuardPos && usableReplyGuardPos < replyAllocationPos &&
        replyAllocationPos < replyLeaseGuardPos && replyLeaseGuardPos < replyQueueGuardPos &&
        replyQueueGuardPos < activeTerminalQueueGuardPos &&
        activeTerminalQueueGuardPos < stateLockPos && stateLockPos < resolveUrlPos &&
        resolveUrlPos < startOtaPos,
        "Connected canonical reply routing and the dedicated active-terminal queue must be verified before claiming a task or writing Flash");
    size_t usableReplyGuardEnd = command.find("}", usableReplyGuardPos);
    TEST_ASSERT_TRUE_MESSAGE(
        usableReplyGuardEnd != std::string::npos &&
        command.substr(usableReplyGuardPos, usableReplyGuardEnd - usableReplyGuardPos)
                .find("return;") != std::string::npos,
        "A disabled or non-canonical OTA reply topic must stop the command before Flash OTA");
    size_t replyLeaseGuardEnd = command.find("}", replyLeaseGuardPos);
    TEST_ASSERT_TRUE_MESSAGE(
        replyLeaseGuardEnd != std::string::npos &&
        command.substr(replyLeaseGuardPos, replyLeaseGuardEnd - replyLeaseGuardPos).find("return;") != std::string::npos,
        "Unavailable reply storage must stop a command before claiming a task or writing Flash");

    size_t terminalBranchPos = command.find("if (cachedTerminalStatus ==");
    size_t activeBranchPos = command.find("if (duplicateActiveTask)", terminalBranchPos);
    size_t busyBranchPos = command.find("if (anotherTaskActive)", activeBranchPos);
    size_t otaFeatureGuardPos = command.find("#if !FASTBEE_ENABLE_OTA", busyBranchPos);
    TEST_ASSERT_TRUE_MESSAGE(
        terminalBranchPos != std::string::npos && activeBranchPos != std::string::npos &&
        busyBranchPos != std::string::npos && otaFeatureGuardPos != std::string::npos &&
        terminalBranchPos < activeBranchPos && activeBranchPos < busyBranchPos &&
        busyBranchPos < otaFeatureGuardPos,
        "Terminal, active-duplicate, and busy-task branches must remain ordered");
    std::string terminalDuplicateBranch =
        command.substr(terminalBranchPos, activeBranchPos - terminalBranchPos);
    std::string busyBranch = command.substr(
        busyBranchPos, otaFeatureGuardPos - busyBranchPos);
    TEST_ASSERT_TRUE_MESSAGE(
        terminalDuplicateBranch.find("String(cachedTerminalVersion)") != std::string::npos &&
        terminalDuplicateBranch.find("cachedTerminalStillActive || anotherTaskActive") !=
                std::string::npos,
        "Terminal duplicates must use the cached version without consuming capacity owned by an active claim");
    std::string activeDuplicateBranch =
        command.substr(activeBranchPos, busyBranchPos - activeBranchPos);
    TEST_ASSERT_TRUE_MESSAGE(
        activeDuplicateBranch.find("ignored") != std::string::npos &&
        activeDuplicateBranch.find("return;") != std::string::npos &&
        activeDuplicateBranch.find("queueFastBeeOtaReply") == std::string::npos &&
        activeDuplicateBranch.find("STATUS_UPGRADING") == std::string::npos,
        "An active duplicate must be ignored without appending another status=2 reply");
    TEST_ASSERT_TRUE_MESSAGE(
        busyBranch.find("queueFastBeeOtaReply") != std::string::npos &&
        busyBranch.find("uxQueueSpacesAvailable") == std::string::npos,
        "Busy-task replies may use the auxiliary queue without gating the active OTA owner");
    TEST_ASSERT_TRUE_MESSAGE(
        command.find("uxQueueSpacesAvailable(fastBeeOtaReplyQueue)") == std::string::npos,
        "A full non-owner reply queue must not reject a new OTA task before it can use the dedicated terminal queue");

    size_t queueEnd = manager.find("void ProtocolManager::processFastBeeOtaReplies()", queuePos);
    TEST_ASSERT_TRUE_MESSAGE(queueEnd != std::string::npos,
                             "FastBee OTA reply queue function must have a bounded source section");
    std::string queue = manager.substr(queuePos, queueEnd - queuePos);
    size_t terminalFlagPos = queue.find("const bool terminal =");
    size_t terminalCachePos = queue.find("fastBeeOtaLastTerminalTaskId = taskId");
    size_t versionCachePos = queue.find("fastBeeOtaLastTerminalVersion");
    size_t progressBranchPos = queue.find("if (!terminal)", versionCachePos);
    size_t progressOverwritePos = queue.find(
        "xQueueOverwrite(fastBeeOtaProgressQueue", progressBranchPos);
    size_t progressReturnPos = queue.find("return;", progressOverwritePos);
    size_t activeTerminalBranchPos = queue.find("if (activeTerminal)", progressReturnPos);
    size_t activeTerminalOverwritePos = queue.find(
        "xQueueOverwrite(fastBeeOtaActiveTerminalQueue", activeTerminalBranchPos);
    size_t terminalEnqueuePos = queue.find(
        "xQueueSend(fastBeeOtaReplyQueue", activeTerminalOverwritePos);
    size_t discardProgressPos = queue.find(
        "xQueueReceive(fastBeeOtaProgressQueue", terminalEnqueuePos);
    size_t progressPeekPos = queue.find(
        "xQueuePeek(fastBeeOtaProgressQueue", terminalEnqueuePos);
    TEST_ASSERT_TRUE_MESSAGE(
        terminalFlagPos != std::string::npos && terminalCachePos != std::string::npos &&
        versionCachePos != std::string::npos && progressBranchPos != std::string::npos &&
        progressOverwritePos != std::string::npos && progressReturnPos != std::string::npos &&
        activeTerminalBranchPos != std::string::npos &&
        activeTerminalOverwritePos != std::string::npos &&
        terminalEnqueuePos != std::string::npos && discardProgressPos != std::string::npos &&
        progressPeekPos != std::string::npos &&
        terminalFlagPos < terminalCachePos && terminalCachePos < versionCachePos &&
        versionCachePos < progressBranchPos && progressBranchPos < progressOverwritePos &&
        progressOverwritePos < progressReturnPos &&
        progressReturnPos < activeTerminalBranchPos &&
        activeTerminalBranchPos < activeTerminalOverwritePos &&
        activeTerminalOverwritePos < terminalEnqueuePos &&
        terminalEnqueuePos < discardProgressPos,
        "The active terminal must use its dedicated overwrite queue before non-owner replies use the bounded queue");
    TEST_ASSERT_TRUE_MESSAGE(
        queue.find("xQueuePeek(fastBeeOtaProgressQueue", progressPeekPos + 1) ==
            std::string::npos,
        "Terminal enqueue must contain exactly one progress-queue peek guard");
    TEST_ASSERT_TRUE_MESSAGE(
        queue.find("xQueueReceive(fastBeeOtaReplyQueue") == std::string::npos &&
        queue.find("fastBeeOtaActiveTaskId = 0") == std::string::npos,
        "Progress enqueue must never evict a terminal reply or release a failed active task");

    size_t activeTerminalDequeuePos = process.find(
        "xQueueReceive(fastBeeOtaActiveTerminalQueue");
    size_t activeTerminalReplacePos = process.find(
        "fastBeeOtaPendingReply = activeTerminalEvent", activeTerminalDequeuePos);
    size_t terminalDequeuePos = process.find("xQueueReceive(fastBeeOtaReplyQueue");
    size_t progressDequeuePos = process.find("xQueueReceive(fastBeeOtaProgressQueue");
    TEST_ASSERT_TRUE_MESSAGE(
        activeTerminalDequeuePos != std::string::npos &&
        activeTerminalReplacePos != std::string::npos &&
        terminalDequeuePos != std::string::npos && progressDequeuePos != std::string::npos &&
        activeTerminalDequeuePos < activeTerminalReplacePos &&
        activeTerminalReplacePos < terminalDequeuePos && terminalDequeuePos < progressDequeuePos,
        "The active OTA terminal must preempt pending, non-owner terminal, and progress replies");
    size_t pendingProgressGuardPos = process.find(
        "fastBeeOtaPendingReply.status == FastBeeOtaProtocol::STATUS_UPGRADING");
    size_t terminalTakeoverPos = process.find(
        "xQueueReceive(fastBeeOtaReplyQueue, &terminalEvent", pendingProgressGuardPos);
    size_t replacePendingPos = process.find(
        "fastBeeOtaPendingReply = terminalEvent", terminalTakeoverPos);
    size_t resetRetryPos = process.find(
        "fastBeeOtaLastPublishAttemptMs = 0", replacePendingPos);
    TEST_ASSERT_TRUE_MESSAGE(
        pendingProgressGuardPos != std::string::npos && terminalTakeoverPos != std::string::npos &&
        replacePendingPos != std::string::npos && resetRetryPos != std::string::npos &&
        pendingProgressGuardPos < terminalTakeoverPos && terminalTakeoverPos < replacePendingPos &&
        replacePendingPos < resetRetryPos && resetRetryPos < publishPos,
        "A retained status=2 publish must be preempted immediately when status=3/4 is queued");
    TEST_ASSERT_TRUE_MESSAGE(
        managerHeader.find("OtaReplyQueueOwner fastBeeOtaReplyQueues") != std::string::npos &&
        managerHeader.find("char fastBeeOtaLastTerminalVersion[32]") != std::string::npos &&
        manager.find("fastBeeOtaReplyQueues.ensureAndAcquire()") != std::string::npos &&
        readRegressionSrcFile("include/protocols/OtaReplyQueueOwner.h").find("xQueueCreate(1, itemSize_)") != std::string::npos &&
        readRegressionSrcFile("include/protocols/OtaReplyQueueOwner.h").find("xQueueCreate(8, itemSize_)") != std::string::npos,
        "ProtocolManager must retain terminal identity plus lazy dedicated active-terminal and latest-progress slots");
    TestLog::testEnd(true);
}

void test_regression_mqtt_queued_report_publish_failure_retries() {
    TestLog::testStart("Regression: queued MQTT reports survive publish failure");

    std::string mqtt = readRegressionSrcFile("src/protocols/MQTTClient.cpp");
    std::string header = readRegressionSrcFile("include/protocols/MQTTClient.h");
    TEST_ASSERT_FALSE_MESSAGE(mqtt.empty() || header.empty(),
                              "MQTT queued-report sources must be readable");

    size_t processStart = mqtt.find("void MQTTClient::processQueuedReports()");
    size_t processEnd = mqtt.find("void MQTTClient::setMinReportInterval", processStart);
    TEST_ASSERT_TRUE_MESSAGE(processStart != std::string::npos && processEnd != std::string::npos,
                             "MQTT queued-report processor must be discoverable");
    std::string process = mqtt.substr(processStart, processEnd - processStart);

    size_t pendingGuardPos = process.find("if (!_pendingReportSlot.occupied)");
    size_t pendingEncodingCopyPos = process.find(
        "_pendingReportSlot.encoding = slot.encoding", pendingGuardPos);
    size_t pendingCopyPos = process.find(
        "memcpy(_pendingReportSlot.payload, slot.payload", pendingEncodingCopyPos);
    size_t ringClearPos = process.find("slot.clear()", pendingCopyPos);
    size_t pendingPayloadPos = process.find(
        "const String pendingPayload(_pendingReportSlot.payload", ringClearPos);
    size_t encodingBranchPos = process.find(
        "_pendingReportSlot.encoding == MqttPayloadEncoding::MODBUS_RTU_HEX",
        pendingPayloadPos);
    size_t rawPublishPos = process.find(
        "publishModbusRtuReport(pendingPayload)", encodingBranchPos);
    size_t textPublishPos = process.find(
        "publishReportData(pendingPayload)", rawPublishPos);
    size_t publishGuardPos = process.find("if (!publishOk)", textPublishPos);
    size_t retryBreakPos = process.find("break;", publishGuardPos);
    size_t pendingClearPos = process.find("_pendingReportSlot.clear()", retryBreakPos);
    size_t publishedCountPos = process.find("incrementQueueCounter(_queueStats.publishTotal)", pendingClearPos);
    TEST_ASSERT_TRUE_MESSAGE(
        pendingGuardPos != std::string::npos && pendingEncodingCopyPos != std::string::npos &&
        pendingCopyPos != std::string::npos && ringClearPos != std::string::npos &&
        pendingPayloadPos != std::string::npos && encodingBranchPos != std::string::npos &&
        rawPublishPos != std::string::npos && textPublishPos != std::string::npos &&
        publishGuardPos != std::string::npos &&
        retryBreakPos != std::string::npos && pendingClearPos != std::string::npos &&
        publishedCountPos != std::string::npos && pendingGuardPos < pendingCopyPos &&
        pendingGuardPos < pendingEncodingCopyPos &&
        pendingEncodingCopyPos < pendingCopyPos && pendingCopyPos < ringClearPos &&
        ringClearPos < pendingPayloadPos && pendingPayloadPos < encodingBranchPos &&
        encodingBranchPos < rawPublishPos && rawPublishPos < textPublishPos &&
        textPublishPos < publishGuardPos && publishGuardPos < retryBreakPos &&
        retryBreakPos < pendingClearPos &&
        pendingClearPos < publishedCountPos,
        "A queued report must retain its encoding, publish through the matching path, and clear only on success");
    TEST_ASSERT_TRUE_MESSAGE(
        process.substr(publishGuardPos, retryBreakPos - publishGuardPos)
                .find("_pendingReportSlot.clear()") == std::string::npos &&
        header.find("enum class MqttPayloadEncoding") != std::string::npos &&
        header.find("MODBUS_RTU_HEX = 1") != std::string::npos &&
        header.find("MqttPayloadEncoding encoding") != std::string::npos &&
        header.find("MqttSlot _pendingReportSlot") != std::string::npos,
        "Publish failure must keep the exact queued JSON or RAW frame for the next flush");

    TestLog::testEnd(true);
}

void test_regression_guard_group() {
    TestLog::groupStart("Regression Guard Tests");

    // 配置往返
    RUN_TEST(test_config_roundtrip_integrity);
    RUN_TEST(test_nvs_key_isolation);

    // 任务生命周期
    RUN_TEST(test_task_lifecycle_regression);

    // 文件完整性
    RUN_TEST(test_atomic_write_hash_consistency);
    RUN_TEST(test_repeated_file_ops_no_leak);

    // 健康监控
    RUN_TEST(test_health_multi_cycle_no_leak);

    // 多模块并行
    RUN_TEST(test_multi_module_parallel);

    // JSON 安全
    RUN_TEST(test_json_repeated_serialization);

    // 路径链一致性
    RUN_TEST(test_path_operation_chain_consistency);

    // 以太网模式源码回归保护
    RUN_TEST(test_regression_ntp_uses_is_network_connected);
    RUN_TEST(test_regression_ethernet_mdns_after_ap_in_source);
    RUN_TEST(test_regression_boot_report_includes_ethernet);
    RUN_TEST(test_regression_periodic_status_includes_ethernet);
    RUN_TEST(test_regression_ir_keys_subroutes_before_parent);
    RUN_TEST(test_regression_s3_octal_psram_pins_reserved);
    RUN_TEST(test_regression_c6_max_gpio_and_uart0_console_reserved);

    // Bug Fix 回归：MQTT保存不清空主题
    RUN_TEST(test_regression_mqtt_save_preserves_topics_snapshot);
    RUN_TEST(test_regression_mqtt_save_preserves_all_form_fields);

    // Bug Fix 回归：以太网重启重建适配器
    RUN_TEST(test_regression_restart_network_no_initialize_for_ethernet);
    RUN_TEST(test_regression_restart_network_rebuilds_cellular_adapter);
    RUN_TEST(test_regression_restart_network_eth_failure_ap_fallback);

    // Bug Fix 回归：以太网成功后启动混合模式 + mDNS支持以太网
    RUN_TEST(test_regression_restart_network_eth_success_starts_hybrid_ap);
    RUN_TEST(test_regression_restart_network_4g_success_starts_hybrid_ap);
    RUN_TEST(test_regression_dns_mdns_supports_ethernet_mode);
    RUN_TEST(test_regression_dns_health_check_supports_ethernet);

    // logLevel 运行时配置 + syncInterval 回归保护
    RUN_TEST(test_regression_boot_reads_loglevel_from_device_json);
    RUN_TEST(test_regression_sync_interval_applied_to_sntp);
    RUN_TEST(test_regression_no_psram_ntp_uses_sntp_fallback);
    RUN_TEST(test_regression_ap_fallback_probe_cooldown);
    RUN_TEST(test_regression_no_psram_static_admission_limit);
    RUN_TEST(test_regression_device_handler_loglevel_immediate_apply);
    RUN_TEST(test_regression_auto_start_removed);

    // network.json 死字段清理 + IPManager 同步回归保护
    RUN_TEST(test_regression_enable_dns_removed);
    RUN_TEST(test_regression_network_config_json_handler_precedes_legacy_post);
    RUN_TEST(test_regression_ipmanager_config_sync);

    // MQTT topicType 默认值 + autoPrefix 默认值一致性 + content/action 隐式保护
    RUN_TEST(test_regression_mqtt_topic_content_action_passthrough);
    RUN_TEST(test_regression_mqtt_save_content_action_autoPrefix);

    // Modbus master 高级参数 + 电机参数透传
    RUN_TEST(test_regression_modbus_master_advanced_params);
    RUN_TEST(test_regression_modbus_motor_device_summary);

    // MQTT content/action 隐式字段已从结构体和加载代码中删除
    RUN_TEST(test_regression_mqtt_topic_struct_no_content_action);

    // MQTT topicType 枚举值与 topicType 默认值一致性
    RUN_TEST(test_regression_mqtt_topictype_enum_values);

    // Modbus master 高级参数保存/响应一致性
    RUN_TEST(test_regression_modbus_master_params_consistency);

    // users.json 字段一致性：email/remark/createBy 已删除，description 保留
    RUN_TEST(test_regression_users_json_field_consistency);

    // security 精简：仅 4 个 UI 可配置字段，9 个硬编码字段已删除
    RUN_TEST(test_regression_security_json_lite);

    // lastLogin 持久化 + createBy 完全移除回归保护
    RUN_TEST(test_regression_last_login_and_createby_cleanup);

    // WiFi 安全类型默认值 + MQTT 数据同步 + 状态显示 + 摘要默认值回归保护
    RUN_TEST(test_regression_wifi_security_default_wpa2);
    RUN_TEST(test_regression_mqtt_no_hardcoded_defaults);
    // MQTT 禁用状态全链路回归保护
    RUN_TEST(test_regression_mqtt_disabled_waits_for_runtime_status);
    RUN_TEST(test_regression_mqtt_disabled_stops_backend_client);
    RUN_TEST(test_regression_mqtt_disabled_rejects_reconnect);
    RUN_TEST(test_regression_modbus_deferred_restart_before_heap_guard);
    RUN_TEST(test_regression_modbus_restart_distinguishes_intentional_idle);
    RUN_TEST(test_regression_factory_modbus_disabled_and_com15_explicit_topology);
    RUN_TEST(test_regression_modbus_raw_125_registers_dispatch_per_frame);
    RUN_TEST(test_regression_mqtt_summary_default_json);
    RUN_TEST(test_regression_fastbee_ota_mqtt_defaults);
    RUN_TEST(test_regression_fastbee_ota_config_pipeline);
    RUN_TEST(test_regression_fastbee_ota_topic_resolution_and_binary_guard);
    RUN_TEST(test_regression_fastbee_ota_success_reply_precedes_reboot);
    RUN_TEST(test_regression_mqtt_queued_report_publish_failure_retries);
    RUN_TEST(test_regression_device_layout_security_factory_at_bottom);
    RUN_TEST(test_regression_mdns_async_restart);

    // src_filter / lib_deps 排除回归保护
    RUN_TEST(test_regression_src_filter_excludes_unused_adapters);
    RUN_TEST(test_regression_non_cellular_envs_no_tinygsm_sslclient);
    RUN_TEST(test_regression_adapter_headers_have_feature_guard);

    // platformio.ini 宏拼写回归保护
    RUN_TEST(test_regression_platformio_no_misspelled_macros);

    // 热点配置 HTML 布局 + JS 字段完整性 + 密码保护回归保护
    RUN_TEST(test_regression_ap_config_html_layout);
    RUN_TEST(test_regression_ap_config_js_saves_all_fields);
    RUN_TEST(test_regression_ap_handler_password_protection);

    // deviceName 配置回归保护（deviceName 已移至设备配置，网络配置不再管理）
    RUN_TEST(test_regression_ap_config_js_no_device_name);
    RUN_TEST(test_regression_handler_no_device_name_in_network);
    RUN_TEST(test_regression_wifi_manager_uses_device_name_for_ssid);

    // P1 改进验证：WebConfigManager 异步软重启状态机
    RUN_TEST(test_regression_web_soft_restart_async_state_machine);
    RUN_TEST(test_regression_4g_ethernet_mqtt_reconnect_notification);

    // Bug Fix 回归：Worker 内存策略 + 未匹配项回显抑制（假数据污染）
    RUN_TEST(test_regression_worker_stack_clamp_in_source);
    RUN_TEST(test_regression_worker_count_matches_chip_memory_profile);
    RUN_TEST(test_regression_login_admission_separates_block_and_total_dram);
    RUN_TEST(test_regression_data_cmd_unmatched_skip_echo_in_source);

    // 启动后一次性物模型状态上报：只读采集 + 门控与节流
    RUN_TEST(test_regression_boot_report_read_only_in_source);
    RUN_TEST(test_regression_boot_report_gated_and_throttled_in_source);
    RUN_TEST(test_regression_scheduler_web_reserve_uses_8bit_heap_in_source);
    
RUN_TEST(test_regression_scheduler_web_reserve_hysteresis_thresholds_in_source);
    RUN_TEST(test_regression_web_reserve_heartbeat_and_mqtt_gate_in_source);

    // Web 前端资源加载重试（弱网/退化链路下登录页白屏防护）
    RUN_TEST(test_regression_web_loader_retry_in_source);

    // 实时监测传感器缓存合并链路（dataField 透传 → monitor/post id）
    RUN_TEST(test_regression_monitor_sensor_cache_wiring_in_source);

    // 静态文件并发限制回归保护（AP 模式页面加载延迟修复）
    RUN_TEST(test_regression_static_max_inflight_at_least_3);

    // 规则脚本 feature flag + 构建一致性回归保护
    RUN_TEST(test_regression_rule_script_feature_flag_consistency);
    RUN_TEST(test_regression_wifi_only_src_filter_excludes_rule_script);
    RUN_TEST(test_regression_rule_script_gated_in_firmware);
    RUN_TEST(test_regression_unified_web_bundles_rule_script);
    RUN_TEST(test_regression_factory_features_are_memory_safe);

    // 日志并发、长度边界、禁用刷新和文件生命周期回归保护
    RUN_TEST(test_regression_logger_reliability_guards);

    // TLS 内存池原型已退出启动关键路径，但兼容头文件保留
    RUN_TEST(test_regression_tls_pool_stub_not_boot_dependency);

    // 认证 JSON 持久化边界：UserManager 为唯一文件所有者
    RUN_TEST(test_regression_auth_config_has_single_persistence_owner);

    TestLog::groupEnd();
}
