/**
 * @file test_runtime_feature_manager.cpp
 * @brief RuntimeFeatureManager 单元测试（计划 7.1）
 *
 * 覆盖: 默认配置生成 / 持久化与重载 / 依赖联动 / JSON 损坏回退 /
 *       资源估算 / 未编译功能保护 / 状态 JSON / 预算联动
 *
 * native 环境不编译 src/，此处直接包含实现文件并用桩替代 LoggerSystem。
 */

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>

// 模拟 unified 全量编译宏（FeatureFlags.h 内均为 #ifndef 保护，可预先定义）
#ifdef FASTBEE_ENABLE_MDNS
#undef FASTBEE_ENABLE_MDNS
#endif
#define FASTBEE_ENABLE_MDNS 1
#ifdef FASTBEE_ENABLE_WEB_SERVER
#undef FASTBEE_ENABLE_WEB_SERVER
#endif
#define FASTBEE_ENABLE_WEB_SERVER 1
#define FASTBEE_ENABLE_LCD 1
#define FASTBEE_ENABLE_LCD_GB2312 1
#define FASTBEE_ENABLE_NEOPIXEL 1
#define FASTBEE_ENABLE_SEVEN_SEGMENT 1
#define FASTBEE_ENABLE_SENSOR_DRIVER 1
#define FASTBEE_ENABLE_DS18B20 1
#define FASTBEE_ENABLE_IR_REMOTE 1
#define FASTBEE_ENABLE_RFID 1
#define FASTBEE_ENABLE_I2C_SENSORS 1
#define FASTBEE_ENABLE_RF_LEARN 1
#define FASTBEE_ENABLE_DS1302 1
#define FASTBEE_ENABLE_LCD1602 1
#define FASTBEE_ENABLE_RULE_SCRIPT 1
#define FASTBEE_ENABLE_OTA 1
#define FASTBEE_ENABLE_OTA_FS 1
// 刻意不定义: LED_SCREEN / COMMAND_SCRIPT / BLE
// —— 用于验证"未编译功能"路径
// 注: MODBUS_SLAVE 特性已从注册表彻底移除（不再是"未编译功能"），
//     由 test_rfm_modbus_slave_removed 专门验证其不存在

#include "core/RuntimeFeatureManager.h"
#include "core/FeatureBudget.h"

// mock Arduino.h 未提供 Stream 类型，而 LoggerSystem.h 的成员需要它
class Stream {
public:
    virtual ~Stream() = default;
};

// 直接纳入被测实现
#include "../src/core/RuntimeFeatureManager.cpp"

// ── LoggerSystem 原生桩（native 不编译 src/systems/LoggerSystem.cpp）─────
LoggerSystem::LoggerSystem()
    : currentLevel(LOG_INFO), outputStream(nullptr), serialEnabled(false),
      fileLoggingEnabled(false), initialized(false), espLogCaptureEnabled(false),
      logFileSizeLimit(0) {}
LoggerSystem::~LoggerSystem() {}
bool LoggerSystem::initialize() { initialized = true; return true; }
void LoggerSystem::setLogLevel(LogLevel level) { currentLevel = level; }
void LoggerSystem::error(const char*, const char*) {}
void LoggerSystem::warning(const char*, const char*) {}
void LoggerSystem::info(const char*, const char*) {}
void LoggerSystem::debug(const char*, const char*) {}
void LoggerSystem::verbose(const char*, const char*) {}

using FastBee::RuntimeFeatureManager;

static void resetMockFs() {
    g_mockFiles.clear();
    g_mockFileTimes.clear();
    ConfigStorage::getInstance().setSaveFailureCount(0);
}

// Test 1: 首次加载生成默认配置（计划 7.1 Test 1）
void test_rfm_first_run_generates_defaults() {
    resetMockFs();
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());
    TEST_ASSERT_TRUE(LittleFS.exists("/config/features.json"));
    // 统一镜像默认只保留管理入口；外设/协议由功能配置显式启用。
    TEST_ASSERT_TRUE(mgr.shouldEnable("webServer"));
    TEST_ASSERT_TRUE(mgr.shouldEnable("mdns"));
    TEST_ASSERT_FALSE(mgr.shouldEnable("lcd"));
    TEST_ASSERT_FALSE(mgr.shouldEnable("neopixel"));
    TEST_ASSERT_FALSE(mgr.shouldEnable("modbus"));
    TEST_ASSERT_FALSE(mgr.shouldEnable("ruleScript"));
    // 高级功能同样默认禁用
    TEST_ASSERT_FALSE(mgr.shouldEnable("irRemote"));
    TEST_ASSERT_FALSE(mgr.shouldEnable("rfid"));
    TEST_ASSERT_FALSE(mgr.shouldEnable("lcdGb2312"));

    JsonDocument saved;
    TEST_ASSERT_FALSE(deserializeJson(saved, g_mockFiles["/config/features.json"]));
    JsonObject savedFeatures = saved["features"];
    TEST_ASSERT_EQUAL(mgr.featureCount(), savedFeatures.size());
    for (size_t i = 0; i < mgr.featureCount(); ++i) {
        const auto* meta = mgr.getFeatureAt(i);
        TEST_ASSERT_NOT_NULL(meta);
        TEST_ASSERT_TRUE(savedFeatures[meta->key]["enabled"].is<bool>());
    }
}

// 旧版稀疏配置首次加载后补全一次；第二次加载完整快照不得重复写盘。
void test_rfm_sparse_config_canonicalized_once() {
    resetMockFs();
    g_mockFiles["/config/features.json"] =
        "{\"version\":1,\"features\":{\"mdns\":{\"enabled\":true},"
        "\"lcd\":{\"enabled\":true}}}";

    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());
    TEST_ASSERT_TRUE(mgr.isUserEnabled("lcd"));
    TEST_ASSERT_TRUE(mgr.isUserEnabled("webServer"));
    TEST_ASSERT_TRUE(mgr.isUserEnabled("mdns"));

    JsonDocument canonical;
    TEST_ASSERT_FALSE(deserializeJson(canonical, g_mockFiles["/config/features.json"]));
    TEST_ASSERT_EQUAL(mgr.featureCount(), canonical["features"].size());
    TEST_ASSERT_TRUE(LittleFS.exists("/config/features.json.bak"));
    const String legacyBackup = g_mockFiles["/config/features.json.bak"];

    TEST_ASSERT_TRUE(mgr.loadConfig());
    TEST_ASSERT_EQUAL_STRING(legacyBackup.c_str(),
                             g_mockFiles["/config/features.json.bak"].c_str());
    TEST_ASSERT_TRUE(mgr.hasExplicitSetting("webServer"));
    TEST_ASSERT_TRUE(mgr.hasExplicitSetting("mdns"));
}

// mDNS 缺失时需等待 NetworkManager 从旧 network.json 迁移，不能先把
// 新默认值写成显式用户配置并覆盖旧值。
void test_rfm_sparse_config_defers_missing_mdns() {
    resetMockFs();
    const String sparse =
        "{\"version\":1,\"features\":{\"lcd\":{\"enabled\":true}}}";
    g_mockFiles["/config/features.json"] = sparse;

    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());
    TEST_ASSERT_TRUE(mgr.isUserEnabled("lcd"));
    TEST_ASSERT_FALSE(mgr.hasExplicitSetting("mdns"));
    TEST_ASSERT_EQUAL_STRING(sparse.c_str(), g_mockFiles["/config/features.json"].c_str());
    TEST_ASSERT_FALSE(LittleFS.exists("/config/features.json.bak"));
}

void test_rfm_update_save_failure_restores_memory_snapshot() {
    resetMockFs();
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());
    TEST_ASSERT_TRUE(mgr.isUserEnabled("mdns"));
    TEST_ASSERT_FALSE(mgr.isUserEnabled("lcd"));

    JsonDocument update;
    update["features"]["mdns"]["enabled"] = false;
    update["features"]["lcd"]["enabled"] = true;
    ConfigStorage::getInstance().setSaveFailureCount(2);  // saveCurrentConfig retries once
    RuntimeFeatureManager::UserConfigSnapshot previous;
    const auto result = mgr.updateAndSave(update, &previous);

    TEST_ASSERT_EQUAL(static_cast<int>(RuntimeFeatureManager::ConfigUpdateResult::SAVE_FAILED),
                      static_cast<int>(result));
    TEST_ASSERT_TRUE(mgr.isUserEnabled("mdns"));
    TEST_ASSERT_FALSE(mgr.isUserEnabled("lcd"));
    TEST_ASSERT_FALSE(mgr.hasExplicitSetting("mdns"));
    TEST_ASSERT_TRUE(mgr.restoreUserConfig(previous));
}

void test_rfm_reset_restores_complete_defaults() {
    resetMockFs();
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());
    TEST_ASSERT_TRUE(mgr.setUserEnabled("mdns", false));
    TEST_ASSERT_TRUE(mgr.setUserEnabled("lcd", true));
    TEST_ASSERT_TRUE(mgr.saveCurrentConfig());

    TEST_ASSERT_TRUE(mgr.resetToDefaults());
    TEST_ASSERT_TRUE(mgr.isUserEnabled("webServer"));
    TEST_ASSERT_TRUE(mgr.isUserEnabled("mdns"));
    TEST_ASSERT_FALSE(mgr.isUserEnabled("lcd"));
    TEST_ASSERT_TRUE(mgr.hasExplicitSetting("mdns"));

    JsonDocument restored;
    TEST_ASSERT_FALSE(deserializeJson(restored, g_mockFiles["/config/features.json"]));
    TEST_ASSERT_EQUAL(mgr.featureCount(), restored["features"].size());
}

void test_rfm_reset_save_failure_preserves_live_state() {
    resetMockFs();
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());
    TEST_ASSERT_TRUE(mgr.setUserEnabled("mdns", false));
    TEST_ASSERT_TRUE(mgr.setUserEnabled("lcd", true));
    TEST_ASSERT_TRUE(mgr.saveCurrentConfig());

    ConfigStorage::getInstance().setSaveFailureCount(2);
    TEST_ASSERT_FALSE(mgr.resetToDefaults());
    TEST_ASSERT_FALSE(mgr.isUserEnabled("mdns"));
    TEST_ASSERT_TRUE(mgr.isUserEnabled("lcd"));
}

// Test 2: 配置持久化与重载
void test_rfm_persist_and_reload() {
    resetMockFs();
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());
    mgr.setNetworkReady(true);

    JsonDocument doc;
    doc["features"]["irRemote"]["enabled"] = true;
    TEST_ASSERT_TRUE(mgr.updateFromJson(doc));
    TEST_ASSERT_TRUE(mgr.shouldEnable("irRemote"));
    TEST_ASSERT_TRUE(mgr.saveCurrentConfig());

    // 重新加载后状态保持
    TEST_ASSERT_TRUE(mgr.loadConfig());
    TEST_ASSERT_TRUE(mgr.shouldEnable("irRemote"));
}

// Test 3: 依赖联动 - lcd 禁用则 lcdGb2312 自动禁用（计划 7.1 Test 2）
void test_rfm_dependency_cascade() {
    resetMockFs();
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());

    JsonDocument doc;
    doc["features"]["lcd"]["enabled"] = false;
    doc["features"]["lcdGb2312"]["enabled"] = true;
    TEST_ASSERT_TRUE(mgr.updateFromJson(doc));
    TEST_ASSERT_FALSE(mgr.shouldEnable("lcd"));
    TEST_ASSERT_FALSE(mgr.shouldEnable("lcdGb2312"));  // 被父功能连带禁用

    // 恢复父功能后子功能可重新启用
    JsonDocument doc2;
    doc2["features"]["lcd"]["enabled"] = true;
    doc2["features"]["lcdGb2312"]["enabled"] = true;
    TEST_ASSERT_TRUE(mgr.updateFromJson(doc2));
    TEST_ASSERT_TRUE(mgr.shouldEnable("lcdGb2312"));
}

// Test 4: JSON 损坏回退到默认配置（计划 7.1 Test 3）
void test_rfm_corrupted_json_fallback() {
    resetMockFs();
    g_mockFiles["/config/features.json"] = "{corrupted: json data!!!";
    auto& mgr = RuntimeFeatureManager::instance();
    // 返回 false 表示使用了回退
    TEST_ASSERT_FALSE(mgr.loadConfig());
    // 回退后仍正常工作，统一镜像默认关闭重型外设
    TEST_ASSERT_FALSE(mgr.shouldEnable("lcd"));
    TEST_ASSERT_FALSE(mgr.shouldEnable("irRemote"));
}

// Test 5: 资源估算正确性（计划 7.1 Test 4）
void test_rfm_estimation() {
    resetMockFs();
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());
    mgr.setNetworkReady(true);
    JsonDocument enable;
    for (const char* key : {"lcd", "neopixel", "sevenSegment", "ds18b20", "modbus", "ruleScript"}) {
        enable["features"][key]["enabled"] = true;
    }
    TEST_ASSERT_TRUE(mgr.updateFromJson(enable));
    // 显式启用后的合计: lcd20+neopixel12+sevenSegment3+ds18b20 5+modbus20+ruleScript 8 = 68KB
    TEST_ASSERT_GREATER_OR_EQUAL(68, mgr.estimateFlashKB());
    TEST_ASSERT_GREATER_THAN(0, mgr.estimateRamB());

    // estimateFromJson 预估新配置
    JsonDocument doc;
    doc["features"]["irRemote"]["enabled"] = true;
    uint32_t flashKB = 0, ramB = 0;
    mgr.estimateFromJson(doc, flashKB, ramB);
    TEST_ASSERT_GREATER_THAN(mgr.estimateFlashKB(), flashKB);
}

// Test 6: 未编译功能不允许通过 API 开启
void test_rfm_uncompiled_cannot_enable() {
    resetMockFs();
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());
    mgr.setNetworkReady(true);

    // ble / commandScript / ledScreen 在当前宏下未编译
    TEST_ASSERT_FALSE(mgr.shouldEnable("ble"));
    TEST_ASSERT_FALSE(mgr.shouldEnable("commandScript"));

    JsonDocument doc;
    doc["features"]["ble"]["enabled"] = true;
    // 仅含未编译功能的更新视为无效（changed=false）
    TEST_ASSERT_FALSE(mgr.updateFromJson(doc));
    TEST_ASSERT_FALSE(mgr.shouldEnable("ble"));
}

// Test 7: 未知功能键安全返回 false
void test_rfm_unknown_key_disabled() {
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_FALSE(mgr.shouldEnable("noSuchFeature"));
    TEST_ASSERT_FALSE(mgr.shouldEnable(nullptr));
}

// Test 8: 状态 JSON 结构（/api/features/status 数据源）
void test_rfm_status_json() {
    resetMockFs();
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());

    JsonDocument doc;
    mgr.getStatusJson(doc.as<JsonVariant>());
    JsonArray features = doc["features"];
    TEST_ASSERT_TRUE(features.size() >= 20);

    // 未编译功能应带 locked 标记
    bool foundLockedBle = false;
    for (JsonObject f : features) {
        if (strcmp(f["key"], "ble") == 0) {
            foundLockedBle = f["locked"] == true;
        }
    }
    TEST_ASSERT_TRUE(foundLockedBle);

    JsonObject summary = doc["summary"];
    TEST_ASSERT_TRUE(summary["totalFlashKB"].as<uint32_t>() > 0);
    TEST_ASSERT_TRUE(summary["activeFlashKB"].as<uint32_t>() > 0);

    // 低内存 Web 路由逐项输出必须与完整状态保持同一字段/激活语义。
    for (size_t i = 0; i < mgr.featureCount(); ++i) {
        JsonDocument itemDoc;
        TEST_ASSERT_TRUE(mgr.getFeatureStatusJson(i, itemDoc.to<JsonObject>()));
        TEST_ASSERT_EQUAL_STRING(features[i]["key"].as<const char*>(),
                                 itemDoc["key"].as<const char*>());
        TEST_ASSERT_EQUAL(features[i]["compiled"].as<bool>(), itemDoc["compiled"].as<bool>());
        TEST_ASSERT_EQUAL(features[i]["userEnabled"].as<bool>(), itemDoc["userEnabled"].as<bool>());
        TEST_ASSERT_EQUAL(features[i]["active"].as<bool>(), itemDoc["active"].as<bool>());
    }
    JsonDocument streamedSummary;
    mgr.getStatusSummaryJson(streamedSummary.to<JsonObject>());
    TEST_ASSERT_EQUAL(summary["totalFlashKB"].as<uint32_t>(),
                      streamedSummary["totalFlashKB"].as<uint32_t>());
    TEST_ASSERT_EQUAL(summary["activeFlashKB"].as<uint32_t>(),
                      streamedSummary["activeFlashKB"].as<uint32_t>());
    TEST_ASSERT_EQUAL(summary["activeRamB"].as<uint32_t>(),
                      streamedSummary["activeRamB"].as<uint32_t>());
    JsonDocument invalidItem;
    TEST_ASSERT_FALSE(mgr.getFeatureStatusJson(mgr.featureCount(),
                                               invalidItem.to<JsonObject>()));
}

// Test 9: 预算拒绝联动 - FeatureBudgetRegistry 拒绝的功能即使启用也不生效
void test_rfm_budget_denied() {
    resetMockFs();
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());
    mgr.setNetworkReady(true);

    JsonDocument doc;
    doc["features"]["rfid"]["enabled"] = true;
    TEST_ASSERT_TRUE(mgr.updateFromJson(doc));
    TEST_ASSERT_TRUE(mgr.shouldEnable("rfid"));

    // 模拟启动预算拒绝 rfid
    FastBee::FeatureBudgetRegistryAccess::deny("rfid");
    TEST_ASSERT_TRUE(mgr.loadConfig());  // 重新同步预算状态
    TEST_ASSERT_FALSE(mgr.shouldEnable("rfid"));
}

// Test 10: modbusSlave 从站特性已彻底移除（禁用从站后不再注册，界面不显示）
void test_rfm_modbus_slave_removed() {
    resetMockFs();
    auto& mgr = RuntimeFeatureManager::instance();
    TEST_ASSERT_TRUE(mgr.loadConfig());

    // 从站特性已从注册表移除：shouldEnable 对未注册 key 安全返回 false
    TEST_ASSERT_FALSE(mgr.shouldEnable("modbusSlave"));

    // 状态 JSON（/api/features/status 数据源）不得含 modbusSlave，
    // 确保功能配置界面彻底不显示从站项（而非 compiled=false 的灰显）
    JsonDocument doc;
    mgr.getStatusJson(doc.as<JsonVariant>());
    JsonArray features = doc["features"];
    bool foundSlave = false;
    bool foundMaster = false;
    for (JsonObject f : features) {
        if (strcmp(f["key"], "modbusSlave") == 0) foundSlave = true;
        if (strcmp(f["key"], "modbus") == 0) foundMaster = true;
    }
    TEST_ASSERT_FALSE(foundSlave);   // 从站项已移除
    TEST_ASSERT_TRUE(foundMaster);   // 主站项 modbus 仍保留
}

void test_runtime_feature_manager_group() {
    RUN_TEST(test_rfm_first_run_generates_defaults);
    RUN_TEST(test_rfm_sparse_config_canonicalized_once);
    RUN_TEST(test_rfm_sparse_config_defers_missing_mdns);
    RUN_TEST(test_rfm_update_save_failure_restores_memory_snapshot);
    RUN_TEST(test_rfm_reset_restores_complete_defaults);
    RUN_TEST(test_rfm_reset_save_failure_preserves_live_state);
    RUN_TEST(test_rfm_persist_and_reload);
    RUN_TEST(test_rfm_dependency_cascade);
    RUN_TEST(test_rfm_corrupted_json_fallback);
    RUN_TEST(test_rfm_estimation);
    RUN_TEST(test_rfm_uncompiled_cannot_enable);
    RUN_TEST(test_rfm_unknown_key_disabled);
    RUN_TEST(test_rfm_status_json);
    RUN_TEST(test_rfm_budget_denied);
    RUN_TEST(test_rfm_modbus_slave_removed);
}
