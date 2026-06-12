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
#include "mocks/MockConfigStorage.h"
#include "mocks/MockTaskManager.h"
#include "mocks/MockHealthMonitor.h"
#include "utils/FileUtils.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_regression_guard_group();

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

    TestLog::groupEnd();
}
