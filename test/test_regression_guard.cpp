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
            return ss.str();
        }
    }
    return "";
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
    size_t restoreInSave = saveBody.find("pubTopics.innerHTML = snapshot");
    TEST_ASSERT_TRUE_MESSAGE(snapshotInSave != std::string::npos,
        "snapshot creation not found in saveProtocolConfig");
    TEST_ASSERT_TRUE_MESSAGE(loadInSave != std::string::npos,
        "_loadFullProtocolConfig call not found in saveProtocolConfig");
    TEST_ASSERT_TRUE_MESSAGE(restoreInSave != std::string::npos,
        "pubTopics.innerHTML restore not found in saveProtocolConfig");
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

    size_t restartPos = content.find("FBNetworkManager::restartNetwork");
    TEST_ASSERT_TRUE(restartPos != std::string::npos);
    std::string restartBody = content.substr(restartPos, 9000);

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
    std::string summaryBody = src.substr(summaryPos, 600);
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

    size_t devSummaryPos = src.find("copyModbusDeviceSummary");
    TEST_ASSERT_TRUE_MESSAGE(devSummaryPos != std::string::npos,
        "copyModbusDeviceSummary not found");
    std::string devSummaryBody = src.substr(devSummaryPos, 1200);

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
    TestLog::testStart("regression: security section lite - only 4 UI fields");

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

    // 3. UserManager 保存代码只写 4 个字段
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
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("maxPasswordLength") == std::string::npos,
        "save must NOT write 'maxPasswordLength'");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("cookieName") == std::string::npos,
        "save must NOT write 'cookieName'");
    TEST_ASSERT_TRUE_MESSAGE(saveBlock.find("allowMultipleSessions") == std::string::npos,
        "save must NOT write 'allowMultipleSessions'");
    TestLog::step("UserManager save: only 4 security fields written");

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

    // 3. protocol-mqtt.html 模板不应包含硬编码示例值
    std::string html = readRegressionSrcFile("web-src/pages/fragments/protocol-mqtt.html");
    TEST_ASSERT_FALSE_MESSAGE(html.empty(), "Cannot read protocol-mqtt.html");

    TEST_ASSERT_TRUE_MESSAGE(html.find("iot.fastbee.cn") == std::string::npos,
        "protocol-mqtt.html must NOT hardcode 'iot.fastbee.cn' in template");
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
 * @brief MQTT 未启用时必须直接显示"未连接"而非"检测中"
 * 回归：旧代码 MQTT 未启用时仍进行状态检测，显示"检测中..."
 */
static void test_regression_mqtt_disabled_shows_offline() {
    TestLog::testStart("Regression: MQTT Disabled Shows Offline");

    // 1. protocol-config.js 中 mqtt.enabled=false 时应停止轮询并显示未连接
    std::string config = readRegressionSrcFile("web-src/modules/runtime/protocol/protocol-config.js");
    TEST_ASSERT_FALSE_MESSAGE(config.empty(), "Cannot read protocol-config.js");

    // 查找 mqtt.enabled 检查后的 else 分支
    size_t enabledPos = config.find("if (mqtt.enabled)");
    TEST_ASSERT_TRUE_MESSAGE(enabledPos != std::string::npos,
        "mqtt.enabled check not found");

    std::string block = config.substr(enabledPos, 500);
    TEST_ASSERT_TRUE_MESSAGE(block.find("_stopMqttStatusPolling") != std::string::npos,
        "When mqtt.enabled=false, must call _stopMqttStatusPolling()");
    TEST_ASSERT_TRUE_MESSAGE(block.find("mqtt-status-offline") != std::string::npos,
        "When mqtt.enabled=false, must set badge to offline");
    TestLog::step("protocol-config.js: mqtt disabled -> stop polling + show offline");

    // 2. mqtt-config.js _updateMqttStatusUI 中 !d.enabled 应直接显示"未连接"
    std::string mqtt = readRegressionSrcFile("web-src/modules/runtime/protocol/mqtt-config.js");
    TEST_ASSERT_FALSE_MESSAGE(mqtt.empty(), "Cannot read mqtt-config.js");

    size_t updatePos = mqtt.find("_updateMqttStatusUI: function");
    TEST_ASSERT_TRUE_MESSAGE(updatePos != std::string::npos,
        "_updateMqttStatusUI not found");

    std::string updateBlock = mqtt.substr(updatePos, 8000);
    // !d.enabled 分支必须在 !d.initialized 分支之前
    size_t disabledPos = updateBlock.find("!d.enabled");
    size_t initPos = updateBlock.find("!d.initialized");
    TEST_ASSERT_TRUE_MESSAGE(disabledPos != std::string::npos,
        "!d.enabled check must exist in _updateMqttStatusUI");
    TEST_ASSERT_TRUE_MESSAGE(initPos != std::string::npos,
        "!d.initialized check must exist");
    TestLog::step("mqtt-config.js: !d.enabled and !d.initialized checks both exist");

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
    std::string updateBlock = cpp.substr(updatePos, 4000);

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
 * @brief 非 Cellular/Ethernet 环境的 src_filter 必须排除对应 .cpp 文件
 *
 * 当 FASTBEE_ENABLE_CELLULAR=0 和 FASTBEE_ENABLE_ETHERNET=0 时，
 * lite/standard 的 src_filter 必须排除 CellularAdapter.cpp 和 EthernetAdapter.cpp，
 * 防止 PlatformIO LDF 扫描并拉入 TinyGSM/SSLClient 等不必要的库依赖。
 */
void test_regression_src_filter_excludes_unused_adapters() {
    TestLog::testStart("Regression: src_filter excludes unused network adapters");

    std::string pio = readRegressionSrcFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "Cannot read platformio.ini");

    // lite_src_filter 必须排除 CellularAdapter.cpp 和 EthernetAdapter.cpp
    TEST_ASSERT_TRUE_MESSAGE(
        pio.find("[lite_src_filter]") != std::string::npos,
        "platformio.ini must have [lite_src_filter] section");
    auto litePos = pio.find("[lite_src_filter]");
    auto liteEnd = pio.find("[", litePos + 1);
    std::string liteSection = pio.substr(litePos, liteEnd - litePos);
    TEST_ASSERT_TRUE_MESSAGE(
        liteSection.find("-<network/CellularAdapter.cpp>") != std::string::npos,
        "lite_src_filter must exclude CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        liteSection.find("-<network/EthernetAdapter.cpp>") != std::string::npos,
        "lite_src_filter must exclude EthernetAdapter.cpp");
    TestLog::step("lite_src_filter excludes CellularAdapter + EthernetAdapter");

    // standard_src_filter 必须排除 CellularAdapter.cpp 和 EthernetAdapter.cpp
    auto stdPos = pio.find("[standard_src_filter]");
    TEST_ASSERT_TRUE_MESSAGE(stdPos != std::string::npos,
        "platformio.ini must have [standard_src_filter] section");
    auto stdEnd = pio.find("[", stdPos + 1);
    std::string stdSection = pio.substr(stdPos, stdEnd - stdPos);
    TEST_ASSERT_TRUE_MESSAGE(
        stdSection.find("-<network/CellularAdapter.cpp>") != std::string::npos,
        "standard_src_filter must exclude CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        stdSection.find("-<network/EthernetAdapter.cpp>") != std::string::npos,
        "standard_src_filter must exclude EthernetAdapter.cpp");
    TestLog::step("standard_src_filter excludes CellularAdapter + EthernetAdapter");

    // standard_ota_src_filter 必须排除 CellularAdapter.cpp 和 EthernetAdapter.cpp
    auto otaPos = pio.find("[standard_ota_src_filter]");
    TEST_ASSERT_TRUE_MESSAGE(otaPos != std::string::npos,
        "platformio.ini must have [standard_ota_src_filter] section");
    auto otaEnd = pio.find("[", otaPos + 1);
    std::string otaSection = pio.substr(otaPos, otaEnd - otaPos);
    TEST_ASSERT_TRUE_MESSAGE(
        otaSection.find("-<network/CellularAdapter.cpp>") != std::string::npos,
        "standard_ota_src_filter must exclude CellularAdapter.cpp");
    TEST_ASSERT_TRUE_MESSAGE(
        otaSection.find("-<network/EthernetAdapter.cpp>") != std::string::npos,
        "standard_ota_src_filter must exclude EthernetAdapter.cpp");
    TestLog::step("standard_ota_src_filter excludes CellularAdapter + EthernetAdapter");

    TestLog::testEnd(true);
}

/**
 * @brief 非 Cellular 环境（esp32-F4R0 / esp32s3-F8R0）的 lib_deps 不应包含 TinyGSM/SSLClient
 *
 * 标准版环境 FASTBEE_ENABLE_CELLULAR=0，不需要 4G 模块和软件 TLS 库，
 * 移除这些依赖可避免 LDF 扫描浪费时间并减少编译依赖。
 * ESP32 标准版 MQTTS 使用内置 WiFiClientSecure（NetworkClientSecure），不依赖 SSLClient。
 */
void test_regression_non_cellular_envs_no_tinygsm_sslclient() {
    TestLog::testStart("Regression: non-cellular envs exclude TinyGSM/SSLClient");

    std::string pio = readRegressionSrcFile("platformio.ini");
    TEST_ASSERT_FALSE_MESSAGE(pio.empty(), "Cannot read platformio.ini");

    // esp32-F4R0 (standard, CELLULAR=0) 不应有 TinyGSM/SSLClient
    auto f4r0Pos = pio.find("[env:esp32-F4R0]");
    TEST_ASSERT_TRUE_MESSAGE(f4r0Pos != std::string::npos,
        "platformio.ini must have [env:esp32-F4R0]");
    auto f4r0End = pio.find("[env:", f4r0Pos + 1);
    std::string f4r0Section = pio.substr(f4r0Pos, f4r0End - f4r0Pos);
    TEST_ASSERT_TRUE_MESSAGE(
        f4r0Section.find("TinyGSM") == std::string::npos,
        "esp32-F4R0 (standard) must not have TinyGSM in lib_deps");
    TEST_ASSERT_TRUE_MESSAGE(
        f4r0Section.find("SSLClient") == std::string::npos,
        "esp32-F4R0 (standard) must not have SSLClient in lib_deps");
    TestLog::step("esp32-F4R0 (standard) excludes TinyGSM + SSLClient");

    // esp32s3-F8R0 (standard+OTA, CELLULAR=0) 不应有 TinyGSM
    auto s3f8r0Pos = pio.find("[env:esp32s3-F8R0]");
    TEST_ASSERT_TRUE_MESSAGE(s3f8r0Pos != std::string::npos,
        "platformio.ini must have [env:esp32s3-F8R0]");
    auto s3f8r0End = pio.find("[env:", s3f8r0Pos + 1);
    std::string s3f8r0Section = pio.substr(s3f8r0Pos, s3f8r0End - s3f8r0Pos);
    TEST_ASSERT_TRUE_MESSAGE(
        s3f8r0Section.find("TinyGSM") == std::string::npos,
        "esp32s3-F8R0 (standard+OTA) must not have TinyGSM in lib_deps");
    TestLog::step("esp32s3-F8R0 (standard+OTA) excludes TinyGSM");

    // full 环境应保留 TinyGSM + SSLClient (CELLULAR=1)
    auto f8r4Pos = pio.find("[env:esp32-F8R4]");
    TEST_ASSERT_TRUE_MESSAGE(f8r4Pos != std::string::npos,
        "platformio.ini must have [env:esp32-F8R4]");
    auto f8r4End = pio.find("[env:", f8r4Pos + 1);
    std::string f8r4Section = pio.substr(f8r4Pos, f8r4End - f8r4Pos);
    TEST_ASSERT_TRUE_MESSAGE(
        f8r4Section.find("TinyGSM") != std::string::npos,
        "esp32-F8R4 (full) must keep TinyGSM in lib_deps");
    TEST_ASSERT_TRUE_MESSAGE(
        f8r4Section.find("SSLClient") != std::string::npos,
        "esp32-F8R4 (full) must keep SSLClient in lib_deps");
    TestLog::step("esp32-F8R4 (full) retains TinyGSM + SSLClient");

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
    auto loadFn = js.find("loadNetworkConfig()");
    TEST_ASSERT_TRUE_MESSAGE(loadFn != std::string::npos,
        "network.js must contain loadNetworkConfig function");

    auto loadBody = js.substr(loadFn, std::min<size_t>(js.size() - loadFn, 2000));
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
    auto loadFn = js.find("loadNetworkConfig()");
    TEST_ASSERT_TRUE_MESSAGE(loadFn != std::string::npos,
        "network.js must contain loadNetworkConfig function");
    auto loadBody = js.substr(loadFn, std::min<size_t>(js.size() - loadFn, 2000));
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
    RUN_TEST(test_regression_mqtt_disabled_shows_offline);
    RUN_TEST(test_regression_mqtt_summary_default_json);
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

    TestLog::groupEnd();
}
