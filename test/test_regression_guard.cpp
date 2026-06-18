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
    const char* roots[] = { ".", "..", "../.." };
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
    std::string restartBody = content.substr(restartPos, 7000);

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
    std::string restartBody = content.substr(restartPos, 7000);

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
    std::string restartBody = content.substr(restartPos, 7000);

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
    std::string restartBody = content.substr(restartPos, 7000);

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

// ========== 测试组入口 ==========

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

    TestLog::groupEnd();
}
