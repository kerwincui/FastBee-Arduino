/**
 * @file test_config_storage.cpp
 * @brief 配置存储单元测试（基于 MockConfigStorage）
 *
 * 覆盖范围：
 *  - NVS 键值 CRUD（String / Int / Bool / Float）
 *  - JSON 配置文件保存/加载
 *  - 配置存在性检查 & 删除
 *  - 备份/恢复
 *  - 导入/导出 & JSON 损坏恢复
 *  - 配置版本管理 & 迁移
 *  - 存储空间追踪
 *  - 批量清理
 */

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "mocks/MockConfigStorage.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_config_storage_group();

// ========== NVS 键值 CRUD ==========

void test_nvs_string_operations() {
    TestLog::testStart("NVS: String CRUD");

    auto& store = MockConfigStore;
    store.initialize();
    store.clearNVS();

    // 写入
    TEST_ASSERT_TRUE(store.putString("wifi_ssid", "MyNetwork"));
    TEST_ASSERT_TRUE(store.putString("wifi_pass", "Secret123"));
    TestLog::step("putString: wifi_ssid, wifi_pass");

    // 读取
    TEST_ASSERT_EQUAL_STRING("MyNetwork", store.getString("wifi_ssid").c_str());
    TEST_ASSERT_EQUAL_STRING("Secret123", store.getString("wifi_pass").c_str());
    TestLog::step("getString: values match");

    // 不存在的键返回默认值
    TEST_ASSERT_EQUAL_STRING("default", store.getString("missing_key", "default").c_str());
    TestLog::step("Missing key returns default value");

    // 覆盖写入
    store.putString("wifi_ssid", "NewNetwork");
    TEST_ASSERT_EQUAL_STRING("NewNetwork", store.getString("wifi_ssid").c_str());
    TestLog::step("Overwrite verified");

    // 删除
    TEST_ASSERT_TRUE(store.removeKey("wifi_ssid"));
    TEST_ASSERT_FALSE(store.exists("wifi_ssid"));
    TEST_ASSERT_EQUAL_STRING("", store.getString("wifi_ssid").c_str());
    TestLog::step("removeKey: key deleted, exists=false");

    // 删除不存在的键
    TEST_ASSERT_FALSE(store.removeKey("nonexistent"));
    TestLog::step("removeKey nonexistent returns false");

    TestLog::testEnd(true);
}

void test_nvs_int_operations() {
    TestLog::testStart("NVS: Int CRUD");

    auto& store = MockConfigStore;
    store.clearNVS();

    TEST_ASSERT_TRUE(store.putInt("mqtt_port", 1883));
    TEST_ASSERT_EQUAL(1883, store.getInt("mqtt_port"));
    TestLog::step("putInt/getInt: 1883");

    store.putInt("mqtt_port", 8883);
    TEST_ASSERT_EQUAL(8883, store.getInt("mqtt_port"));
    TestLog::step("Overwrite int: 8883");

    TEST_ASSERT_EQUAL(0, store.getInt("missing", 0));
    TEST_ASSERT_EQUAL(-1, store.getInt("missing", -1));
    TestLog::step("Missing int returns default");

    TestLog::testEnd(true);
}

void test_nvs_bool_operations() {
    TestLog::testStart("NVS: Bool CRUD");

    auto& store = MockConfigStore;
    store.clearNVS();

    TEST_ASSERT_TRUE(store.putBool("auth_enabled", true));
    TEST_ASSERT_TRUE(store.getBool("auth_enabled"));
    TestLog::step("putBool/getBool: true");

    store.putBool("auth_enabled", false);
    TEST_ASSERT_FALSE(store.getBool("auth_enabled"));
    TestLog::step("Overwrite bool: false");

    TEST_ASSERT_FALSE(store.getBool("missing", false));
    TEST_ASSERT_TRUE(store.getBool("missing", true));
    TestLog::step("Missing bool returns default");

    TestLog::testEnd(true);
}

void test_nvs_float_operations() {
    TestLog::testStart("NVS: Float CRUD");

    auto& store = MockConfigStore;
    store.clearNVS();

    TEST_ASSERT_TRUE(store.putFloat("temp_offset", 1.5f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.5f, store.getFloat("temp_offset"));
    TestLog::step("putFloat/getFloat: 1.5");

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, store.getFloat("missing", 0.0f));
    TestLog::step("Missing float returns default");

    TestLog::testEnd(true);
}

// ========== JSON 配置文件 ==========

void test_json_config_save_load() {
    TestLog::testStart("JSON Config: Save & Load");

    auto& store = MockConfigStore;
    store.initialize();
    store.clearJSONFiles();

    JsonDocument doc;
    doc["ssid"] = "TestWiFi";
    doc["password"] = "pass123";
    doc["dhcp"] = true;
    doc["ip"] = "192.168.1.100";

    TEST_ASSERT_TRUE(store.saveConfig("/config/network.json", doc));
    TestLog::step("saveConfig succeeded");

    TEST_ASSERT_TRUE(store.configExists("/config/network.json"));
    TestLog::step("configExists=true after save");

    JsonDocument loaded;
    TEST_ASSERT_TRUE(store.loadConfig("/config/network.json", loaded));
    TEST_ASSERT_EQUAL_STRING("TestWiFi", loaded["ssid"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("pass123", loaded["password"].as<const char*>());
    TEST_ASSERT_TRUE(loaded["dhcp"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", loaded["ip"].as<const char*>());
    TestLog::step("loadConfig: all fields match");

    TestLog::testEnd(true);
}

void test_json_config_delete() {
    TestLog::testStart("JSON Config: Delete");

    auto& store = MockConfigStore;
    store.clearJSONFiles();

    JsonDocument doc;
    doc["key"] = "value";
    store.saveConfig("/config/test.json", doc);

    TEST_ASSERT_TRUE(store.configExists("/config/test.json"));
    TEST_ASSERT_TRUE(store.deleteConfig("/config/test.json"));
    TEST_ASSERT_FALSE(store.configExists("/config/test.json"));
    TestLog::step("deleteConfig: config removed");

    // 删除不存在的配置
    TEST_ASSERT_FALSE(store.deleteConfig("/config/nonexistent.json"));
    TestLog::step("deleteConfig nonexistent returns false");

    TestLog::testEnd(true);
}

// ========== 备份 & 恢复 ==========

void test_config_backup_restore() {
    TestLog::testStart("Config: Backup & Restore");

    auto& store = MockConfigStore;
    store.clearJSONFiles();

    JsonDocument doc;
    doc["version"] = 1;
    doc["name"] = "original";
    store.saveConfig("/config/device.json", doc);

    // 备份
    TEST_ASSERT_TRUE(store.backupConfig("/config/device.json", "/backup/device.json.bak"));
    TEST_ASSERT_TRUE(store.configExists("/backup/device.json.bak"));
    TestLog::step("Backup created");

    // 修改原文件
    JsonDocument modified;
    modified["version"] = 2;
    modified["name"] = "modified";
    store.saveConfig("/config/device.json", modified);

    // 验证已修改
    JsonDocument check;
    store.loadConfig("/config/device.json", check);
    TEST_ASSERT_EQUAL_STRING("modified", check["name"].as<const char*>());
    TestLog::step("Original modified");

    // 恢复
    TEST_ASSERT_TRUE(store.restoreConfig("/backup/device.json.bak", "/config/device.json"));
    JsonDocument restored;
    store.loadConfig("/config/device.json", restored);
    TEST_ASSERT_EQUAL(1, restored["version"].as<int>());
    TEST_ASSERT_EQUAL_STRING("original", restored["name"].as<const char*>());
    TestLog::step("Restore from backup: original content recovered");

    TestLog::testEnd(true);
}

// ========== JSON 损坏恢复 ==========

void test_json_corruption_recovery() {
    TestLog::testStart("Config: Corruption Recovery");

    auto& store = MockConfigStore;
    store.clearJSONFiles();

    // 先保存正常配置和备份
    JsonDocument doc;
    doc["ssid"] = "SafeWiFi";
    store.saveConfig("/config/network.json", doc);
    store.backupConfig("/config/network.json", "/config/network.json.bak");

    // 模拟损坏
    store.simulateCorruption("/config/network.json");

    // 损坏的文件加载失败
    JsonDocument badDoc;
    TEST_ASSERT_FALSE(store.loadConfig("/config/network.json", badDoc));
    TestLog::step("Corrupted JSON fails to load");

    // 但备份可以加载
    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(store.loadConfig("/config/network.json.bak", backupDoc));
    TEST_ASSERT_EQUAL_STRING("SafeWiFi", backupDoc["ssid"].as<const char*>());
    TestLog::step("Backup fallback: data recovered from .bak");

    TestLog::testEnd(true);
}

// ========== 导入/导出 ==========

void test_config_import_export() {
    TestLog::testStart("Config: Import & Export");

    auto& store = MockConfigStore;
    store.clearJSONFiles();

    JsonDocument doc;
    doc["mqtt"]["server"] = "broker.test.com";
    doc["mqtt"]["port"] = 1883;
    store.saveConfig("/config/protocol.json", doc);

    // 导出
    String exported = store.exportConfig("/config/protocol.json");
    TEST_ASSERT_FALSE(exported.isEmpty());
    TestLog::step("exportConfig: non-empty JSON string");

    // 导入到新路径
    TEST_ASSERT_TRUE(store.importConfig("/config/protocol_v2.json", exported));
    TEST_ASSERT_TRUE(store.configExists("/config/protocol_v2.json"));
    TestLog::step("importConfig: new config created");

    // 验证内容一致
    JsonDocument v2;
    store.loadConfig("/config/protocol_v2.json", v2);
    TEST_ASSERT_EQUAL_STRING("broker.test.com", v2["mqtt"]["server"].as<const char*>());
    TEST_ASSERT_EQUAL(1883, v2["mqtt"]["port"].as<int>());
    TestLog::step("Imported config content matches original");

    // 导入无效 JSON
    TEST_ASSERT_FALSE(store.importConfig("/config/bad.json", "{invalid"));
    TestLog::step("Import invalid JSON rejected");

    TestLog::testEnd(true);
}

// ========== 版本管理 ==========

void test_config_version_management() {
    TestLog::testStart("Config: Version Management");

    auto& store = MockConfigStore;
    store.initialize();

    ConfigVersion v = store.getConfigVersion();
    TEST_ASSERT_EQUAL(1, v.major);
    TEST_ASSERT_EQUAL(0, v.minor);
    TEST_ASSERT_EQUAL(0, v.patch);
    TestLog::step("Default version: 1.0.0");

    ConfigVersion newV(2, 1, 0);
    store.setConfigVersion(newV);
    ConfigVersion check = store.getConfigVersion();
    TEST_ASSERT_EQUAL(2, check.major);
    TEST_ASSERT_EQUAL(1, check.minor);
    TestLog::step("Version updated to 2.1.0");

    // 版本字符串
    TEST_ASSERT_EQUAL_STRING("2.1.0", check.toString().c_str());
    TestLog::step("toString: '2.1.0'");

    TestLog::testEnd(true);
}

void test_config_migration() {
    TestLog::testStart("Config: Migration");

    auto& store = MockConfigStore;
    store.initialize();

    ConfigVersion from(1, 0, 0);
    ConfigVersion to(2, 0, 0);

    TEST_ASSERT_TRUE(store.migrateConfig(from, to));
    ConfigVersion current = store.getConfigVersion();
    TEST_ASSERT_EQUAL(2, current.major);
    TestLog::step("Migration 1.0.0 → 2.0.0 succeeded");

    // 反向迁移不应该成功
    TEST_ASSERT_FALSE(store.migrateConfig(to, from));
    TestLog::step("Downgrade migration rejected");

    TestLog::testEnd(true);
}

// ========== 存储空间追踪 ==========

void test_storage_space_tracking() {
    TestLog::testStart("Storage: Space Tracking");

    auto& store = MockConfigStore;
    store.initialize();
    store.clearAll();

    size_t emptyUsed = store.getUsedSpace();
    TEST_ASSERT_EQUAL(0, (int)emptyUsed);
    TestLog::step("Empty storage: used=0");

    // 写入数据
    store.putString("key1", "value1");
    JsonDocument doc;
    doc["data"] = "test";
    store.saveConfig("/test.json", doc);

    size_t afterUsed = store.getUsedSpace();
    TEST_ASSERT_GREATER_THAN((int)emptyUsed, (int)afterUsed);
    TestLog::step("Used space increased after writes");

    size_t freeSpace = store.getFreeSpace();
    TEST_ASSERT_GREATER_THAN(0, (int)freeSpace);
    TestLog::step("Free space > 0");

    TestLog::testEnd(true);
}

// ========== 批量清理 ==========

void test_clear_operations() {
    TestLog::testStart("Clear Operations");

    auto& store = MockConfigStore;
    store.initialize();

    store.putString("k1", "v1");
    store.putString("k2", "v2");
    JsonDocument doc;
    doc["a"] = 1;
    store.saveConfig("/a.json", doc);
    store.saveConfig("/b.json", doc);

    // 只清 NVS
    store.clearNVS();
    TEST_ASSERT_FALSE(store.exists("k1"));
    TEST_ASSERT_TRUE(store.configExists("/a.json"));
    TestLog::step("clearNVS: NVS cleared, JSON files preserved");

    // 只清 JSON
    store.putString("k1", "v1");  // 重新写入
    store.clearJSONFiles();
    TEST_ASSERT_TRUE(store.exists("k1"));
    TEST_ASSERT_FALSE(store.configExists("/a.json"));
    TestLog::step("clearJSONFiles: JSON cleared, NVS preserved");

    // 全部清理
    store.putString("k1", "v1");
    store.saveConfig("/a.json", doc);
    store.clearAll();
    TEST_ASSERT_FALSE(store.exists("k1"));
    TEST_ASSERT_FALSE(store.configExists("/a.json"));
    TestLog::step("clearAll: everything cleared");

    TestLog::testEnd(true);
}

// ========== 键枚举 ==========

void test_get_all_keys() {
    TestLog::testStart("NVS: Get All Keys");

    auto& store = MockConfigStore;
    store.clearNVS();

    store.putString("alpha", "1");
    store.putString("beta", "2");
    store.putInt("gamma", 3);

    std::vector<String> keys = store.getAllKeys();
    TEST_ASSERT_EQUAL(3, keys.size());
    TestLog::step("getAllKeys: 3 keys found");

    // 验证包含预期键
    bool hasAlpha = false, hasBeta = false, hasGamma = false;
    for (auto& k : keys) {
        if (k == "alpha") hasAlpha = true;
        if (k == "beta") hasBeta = true;
        if (k == "gamma") hasGamma = true;
    }
    TEST_ASSERT_TRUE(hasAlpha);
    TEST_ASSERT_TRUE(hasBeta);
    TEST_ASSERT_TRUE(hasGamma);
    TestLog::step("All expected keys present");

    TestLog::testEnd(true);
}

// ========== 测试组入口 ==========

void test_config_storage_group() {
    TestLog::groupStart("ConfigStorage Tests");

    // NVS CRUD
    RUN_TEST(test_nvs_string_operations);
    RUN_TEST(test_nvs_int_operations);
    RUN_TEST(test_nvs_bool_operations);
    RUN_TEST(test_nvs_float_operations);

    // JSON 配置
    RUN_TEST(test_json_config_save_load);
    RUN_TEST(test_json_config_delete);

    // 备份恢复
    RUN_TEST(test_config_backup_restore);
    RUN_TEST(test_json_corruption_recovery);

    // 导入导出
    RUN_TEST(test_config_import_export);

    // 版本管理
    RUN_TEST(test_config_version_management);
    RUN_TEST(test_config_migration);

    // 空间追踪
    RUN_TEST(test_storage_space_tracking);

    // 清理
    RUN_TEST(test_clear_operations);
    RUN_TEST(test_get_all_keys);

    TestLog::groupEnd();
}
