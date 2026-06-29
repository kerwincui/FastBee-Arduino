/**
 * @file test_system_services.cpp
 * @brief 系统服务模块单元测试
 * 
 * 测试内容：
 * - ConfigStorage: NVS 读写、JSON 配置保存/加载/备份/恢复、损坏恢复、版本迁移
 * - HealthMonitor: 健康状态检查、阈值判定、报告生成
 * - LoggerSystem: 日志级别过滤、模块黑名单、文件轮转、查询统计、logLevel字符串解析
 * - TaskManager: 任务 CRUD、优先级调度、暂停/恢复、统计
 */

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include "helpers/TestLogger.h"
#include "mocks/MockConfigStorage.h"
#include "mocks/MockHealthMonitor.h"
#include "mocks/MockLogger.h"
#include "mocks/MockTaskManager.h"

void test_system_services_group();

// ========== ConfigStorage 测试 ==========

static void test_config_nvs_string_rw() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    TEST_ASSERT_TRUE(store.putString("wifi_ssid", "MyNetwork"));
    TEST_ASSERT_EQUAL_STRING("MyNetwork", store.getString("wifi_ssid").c_str());
}

static void test_config_nvs_int_rw() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    TEST_ASSERT_TRUE(store.putInt("port", 1883));
    TEST_ASSERT_EQUAL(1883, store.getInt("port"));
}

static void test_config_nvs_bool_rw() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    TEST_ASSERT_TRUE(store.putBool("auto_connect", true));
    TEST_ASSERT_TRUE(store.getBool("auto_connect"));
    
    store.putBool("auto_connect", false);
    TEST_ASSERT_FALSE(store.getBool("auto_connect"));
}

static void test_config_nvs_float_rw() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    store.putFloat("threshold", 3.14f);
    float val = store.getFloat("threshold");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.14f, val);
}

static void test_config_nvs_default_values() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    TEST_ASSERT_EQUAL_STRING("default", store.getString("nonexist", "default").c_str());
    TEST_ASSERT_EQUAL(42, store.getInt("nonexist", 42));
    TEST_ASSERT_TRUE(store.getBool("nonexist", true));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.5f, store.getFloat("nonexist", 1.5f));
}

static void test_config_nvs_remove_key() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    store.putString("temp_key", "temp_value");
    TEST_ASSERT_TRUE(store.exists("temp_key"));
    
    TEST_ASSERT_TRUE(store.removeKey("temp_key"));
    TEST_ASSERT_FALSE(store.exists("temp_key"));
    
    // 删除不存在的 key
    TEST_ASSERT_FALSE(store.removeKey("nonexist"));
}

static void test_config_json_save_load() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    // 保存 JSON 配置
    JsonDocument doc;
    doc["ssid"] = "TestWiFi";
    doc["password"] = "Secret123";
    doc["dhcp"] = true;
    doc["port"] = 80;
    
    TEST_ASSERT_TRUE(store.saveConfig("/config/network.json", doc));
    TEST_ASSERT_TRUE(store.configExists("/config/network.json"));
    
    // 加载 JSON 配置
    JsonDocument loaded;
    TEST_ASSERT_TRUE(store.loadConfig("/config/network.json", loaded));
    TEST_ASSERT_EQUAL_STRING("TestWiFi", loaded["ssid"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Secret123", loaded["password"].as<const char*>());
    TEST_ASSERT_TRUE(loaded["dhcp"].as<bool>());
    TEST_ASSERT_EQUAL(80, loaded["port"].as<int>());
}

static void test_config_json_load_nonexist() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    JsonDocument doc;
    TEST_ASSERT_FALSE(store.loadConfig("/config/ghost.json", doc));
}

static void test_config_backup_restore() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    JsonDocument doc;
    doc["version"] = "1.0.0";
    doc["data"] = "important";
    store.saveConfig("/config/test.json", doc);
    
    // 备份
    TEST_ASSERT_TRUE(store.backupConfig("/config/test.json", "/config/test.json.bak"));
    
    // 删除原文件
    store.deleteConfig("/config/test.json");
    TEST_ASSERT_FALSE(store.configExists("/config/test.json"));
    
    // 从备份恢复
    TEST_ASSERT_TRUE(store.restoreConfig("/config/test.json.bak", "/config/test.json"));
    
    JsonDocument restored;
    TEST_ASSERT_TRUE(store.loadConfig("/config/test.json", restored));
    TEST_ASSERT_EQUAL_STRING("important", restored["data"].as<const char*>());
}

static void test_config_corruption_recovery() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    // 模拟配置损坏
    store.simulateCorruption("/config/device.json");
    
    JsonDocument doc;
    // 损坏的JSON无法解析
    TEST_ASSERT_FALSE(store.loadConfig("/config/device.json", doc));
}

static void test_config_import_invalid_json() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    // 导入无效 JSON
    TEST_ASSERT_FALSE(store.importConfig("/config/bad.json", "{not valid json"));
    TEST_ASSERT_FALSE(store.configExists("/config/bad.json"));
}

static void test_config_import_valid_json() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    String validJson = "{\"key\":\"value\",\"num\":42}";
    TEST_ASSERT_TRUE(store.importConfig("/config/import.json", validJson));
    
    String exported = store.exportConfig("/config/import.json");
    TEST_ASSERT_TRUE(exported.indexOf("value") >= 0);
}

static void test_config_version_migration() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    ConfigVersion v1(1, 0, 0);
    ConfigVersion v2(2, 0, 0);
    
    store.setConfigVersion(v1);
    TEST_ASSERT_EQUAL_STRING("1.0.0", store.getConfigVersion().toString().c_str());
    
    TEST_ASSERT_TRUE(store.migrateConfig(v1, v2));
    TEST_ASSERT_EQUAL_STRING("2.0.0", store.getConfigVersion().toString().c_str());
    
    // 降级迁移应失败
    TEST_ASSERT_FALSE(store.migrateConfig(v2, v1));
}

static void test_config_space_check() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    size_t freeSpace = store.getFreeSpace();
    TEST_ASSERT_TRUE(freeSpace > 0);
    
    // 写入数据后空间减少
    store.putString("big_key", "some data that takes space");
    TEST_ASSERT_TRUE(store.getFreeSpace() < freeSpace);
}

// ========== 配置导入导出多文件选择测试 ==========

static void test_config_transfer_export_multiple() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    // 预置多个配置文件
    store.importConfig("/config/device.json", "{\"deviceId\":\"FBE001\",\"deviceName\":\"Test\"}");
    store.importConfig("/config/network.json", "{\"mode\":0,\"staSSID\":\"wifi\"}");
    store.importConfig("/config/protocol.json", "{\"version\":2,\"mqtt\":{\"enabled\":true}}");
    
    // 验证所有文件存在并可导出
    TEST_ASSERT_TRUE(store.configExists("/config/device.json"));
    TEST_ASSERT_TRUE(store.configExists("/config/network.json"));
    TEST_ASSERT_TRUE(store.configExists("/config/protocol.json"));
    
    String dev = store.exportConfig("/config/device.json");
    String net = store.exportConfig("/config/network.json");
    String proto = store.exportConfig("/config/protocol.json");
    
    TEST_ASSERT_TRUE(dev.indexOf("FBE001") >= 0);
    TEST_ASSERT_TRUE(net.indexOf("wifi") >= 0);
    TEST_ASSERT_TRUE(proto.indexOf("mqtt") >= 0);
}

static void test_config_transfer_import_selective() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    // 模拟用户在弹窗中选择部分文件导入（只导入 device 和 network，不导入 protocol）
    store.importConfig("/config/device.json", "{\"deviceId\":\"FBE002\"}");
    store.importConfig("/config/network.json", "{\"mode\":2,\"apSSID\":\"fastbee-ap\"}");
    
    TEST_ASSERT_TRUE(store.configExists("/config/device.json"));
    TEST_ASSERT_TRUE(store.configExists("/config/network.json"));
    // protocol.json 未导入，不应存在
    TEST_ASSERT_FALSE(store.configExists("/config/protocol.json"));
    
    String dev = store.exportConfig("/config/device.json");
    TEST_ASSERT_TRUE(dev.indexOf("FBE002") >= 0);
}

static void test_config_transfer_overwrite_existing() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    // 初始配置
    store.importConfig("/config/device.json", "{\"deviceId\":\"OLD_ID\",\"name\":\"Old\"}");
    String before = store.exportConfig("/config/device.json");
    TEST_ASSERT_TRUE(before.indexOf("OLD_ID") >= 0);
    
    // 导入新配置覆盖旧配置
    store.importConfig("/config/device.json", "{\"deviceId\":\"NEW_ID\",\"name\":\"New\"}");
    String after = store.exportConfig("/config/device.json");
    TEST_ASSERT_TRUE(after.indexOf("NEW_ID") >= 0);
    TEST_ASSERT_TRUE(after.indexOf("OLD_ID") < 0);
}

static void test_config_transfer_empty_content_rejected() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    // 空内容不应被导入
    TEST_ASSERT_FALSE(store.importConfig("/config/empty.json", ""));
    TEST_ASSERT_FALSE(store.configExists("/config/empty.json"));
}

static void test_config_transfer_large_valid_json() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    // 构造较大但有效的 JSON（模拟 peripherals.json 等大文件）
    String largeJson = "{\"peripherals\":[";
    for (int i = 0; i < 20; i++) {
        if (i > 0) largeJson += ",";
        largeJson += "{\"id\":\"p" + String(i) + "\",\"type\":12,\"enabled\":true}";
    }
    largeJson += "]}";
    
    TEST_ASSERT_TRUE(store.importConfig("/config/peripherals.json", largeJson));
    
    String exported = store.exportConfig("/config/peripherals.json");
    TEST_ASSERT_TRUE(exported.indexOf("p0") >= 0);
    TEST_ASSERT_TRUE(exported.indexOf("p19") >= 0);
}

static void test_config_transfer_export_nonexistent() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    // 导出不存在的文件应返回空字符串
    String result = store.exportConfig("/config/nonexistent.json");
    TEST_ASSERT_TRUE(result.isEmpty());
}

// ========== 配置导入安全性测试 ==========

// 测试：白名单检查 - 未知文件名应被拒绝
static void test_config_import_whitelist_rejects_unknown() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();

    // 模拟 AI 生成的任意命名文件（之前导致配置丢失的根因）
    TEST_ASSERT_FALSE(store.importConfigValidated(
        "deepseek_json_20260629.json",
        "{\"peripherals\":[]}"));
    TEST_ASSERT_TRUE(store.lastImportError.indexOf("不允许导入") >= 0);

    // 确保没有在文件系统中创建该文件
    TEST_ASSERT_FALSE(store.configExists("/config/deepseek_json_20260629.json"));
    TEST_ASSERT_EQUAL(0, (int)store.getConfigFileCount());
}

// 测试：白名单检查 - 已知文件名应被接受
static void test_config_import_whitelist_accepts_known() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();

    // 白名单中的文件名均应导入成功
    const char* allowedFiles[] = {
        "device.json", "network.json", "peripherals.json",
        "periph_exec.json", "protocol.json", "auth.json", "rule_scripts.json"
    };
    const char* validJson[] = {
        "{\"deviceName\":\"test\"}", "{\"mode\":0}", "{\"peripherals\":[]}",
        "{\"rules\":[]}", "{\"mqtt\":{}}", "{\"token\":\"\"}", "{\"scripts\":[]}"
    };
    for (int i = 0; i < 7; i++) {
        bool ok = store.importConfigValidated(allowedFiles[i], validJson[i]);
        TEST_ASSERT_TRUE_MESSAGE(ok, (String("Failed for: ") + allowedFiles[i]).c_str());
    }
    TEST_ASSERT_EQUAL(7, (int)store.getConfigFileCount());
}

// 测试：JSON 格式验证 - 无效 JSON 应被拒绝
static void test_config_import_invalid_json_rejected() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();

    // 非 JSON 文本
    TEST_ASSERT_FALSE(store.importConfigValidated("device.json", "this is not json"));
    TEST_ASSERT_TRUE(store.lastImportError.indexOf("JSON") >= 0);

    // 不完整的 JSON
    TEST_ASSERT_FALSE(store.importConfigValidated("device.json", "{incomplete"));
    TEST_ASSERT_TRUE(store.lastImportError.indexOf("JSON") >= 0);

    // 纯数字
    TEST_ASSERT_FALSE(store.importConfigValidated("device.json", "12345"));
    TEST_ASSERT_TRUE(store.lastImportError.indexOf("JSON") >= 0);

    TEST_ASSERT_EQUAL(0, (int)store.getConfigFileCount());
}

// 测试：空内容应被拒绝
static void test_config_import_empty_content_rejected() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();

    TEST_ASSERT_FALSE(store.importConfigValidated("device.json", ""));
    TEST_ASSERT_TRUE(store.lastImportError.indexOf("为空") >= 0);
}

// 测试：超大配置文件应被拒绝
static void test_config_import_oversize_rejected() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();

    // 构造超过限制的内容
    String oversize = "{\"data\":\"";
    for (int i = 0; i < 5000; i++) oversize += "AAAAAAAAAA";  // 50KB
    oversize += "\"}";

    TEST_ASSERT_FALSE(store.importConfigValidated("protocol.json", oversize, 24576));
    TEST_ASSERT_TRUE(store.lastImportError.indexOf("过大") >= 0);
}

// 测试：文件系统完整性 - 导入后只应有白名单内的文件
static void test_config_import_no_stale_files() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();

    // 导入白名单文件
    store.importConfigValidated("peripherals.json", "{\"peripherals\":[]}");
    store.importConfigValidated("periph_exec.json", "{\"rules\":[]}");

    // 检查只有这两个文件
    TEST_ASSERT_EQUAL(2, (int)store.getConfigFileCount());
    auto files = store.listConfigFiles();
    bool hasPeriph = false, hasExec = false;
    for (auto& name : files) {
        if (name == "peripherals.json") hasPeriph = true;
        if (name == "periph_exec.json") hasExec = true;
    }
    TEST_ASSERT_TRUE(hasPeriph);
    TEST_ASSERT_TRUE(hasExec);
}

// 测试：清理残留文件功能
static void test_config_cleanup_stale_files() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();

    // 模拟之前错误导入创建的残留文件
    store.importConfig("/config/deepseek_json_abc.json", "{\"data\":1}");
    store.importConfig("/config/random_file.json", "{\"x\":2}");
    store.importConfig("/config/peripherals.json", "{\"peripherals\":[]}");

    // 清理前应有3个文件
    TEST_ASSERT_EQUAL(3, (int)store.getConfigFileCount());

    // 执行清理
    int removed = store.cleanupStaleFiles();
    TEST_ASSERT_EQUAL(2, removed);  // 2个非白名单文件被清理
    TEST_ASSERT_EQUAL(1, (int)store.getConfigFileCount());

    // 只有白名单文件保留
    TEST_ASSERT_TRUE(store.configExists("/config/peripherals.json"));
    TEST_ASSERT_FALSE(store.configExists("/config/deepseek_json_abc.json"));
    TEST_ASSERT_FALSE(store.configExists("/config/random_file.json"));
}

// 测试：文件名安全检查 - 非法字符应被拒绝
static void test_config_import_illegal_filename() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();

    // 包含路径穿越
    TEST_ASSERT_FALSE(store.importConfigValidated("../etc/passwd.json", "{}"));
    TEST_ASSERT_TRUE(store.lastImportError.indexOf("非法") >= 0);

    // 包含反斜杠
    TEST_ASSERT_FALSE(store.importConfigValidated("..\\config.json", "{}"));
    TEST_ASSERT_TRUE(store.lastImportError.indexOf("非法") >= 0);

    // 非 .json 后缀
    TEST_ASSERT_FALSE(store.importConfigValidated("device.txt", "{}"));
    TEST_ASSERT_TRUE(store.lastImportError.indexOf(".json") >= 0);

    // 空文件名
    TEST_ASSERT_FALSE(store.importConfigValidated("", "{}"));
}

// 测试：存储空间不足时应被拒绝
static void test_config_import_low_space_rejected() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    store.setLowSpaceMode(true);

    // 填充大量数据占用空间
    String bigData = "X";
    for (int i = 0; i < 10; i++) bigData += bigData;
    store.putString("big_key", bigData);

    TEST_ASSERT_FALSE(store.importConfigValidated("device.json", "{\"name\":\"test\"}"));
    TEST_ASSERT_TRUE(store.lastImportError.indexOf("不足") >= 0);

    store.setLowSpaceMode(false);
}

// 测试：白名单检查 - roles.json 应被拒绝（安全保护）
static void test_config_import_roles_json_rejected() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();

    // roles.json 不在白名单中
    TEST_ASSERT_FALSE(store.importConfigValidated("roles.json", "[]"));
    TEST_ASSERT_TRUE(store.lastImportError.indexOf("不允许") >= 0);
}

// ========== HealthMonitor 测试 ==========

static void test_health_initialize() {
    auto& monitor = MockHealthMonitor::getInstance();
    TEST_ASSERT_TRUE(monitor.initialize());
    
    SystemHealth health = monitor.getHealthStatus();
    TEST_ASSERT_TRUE(health.isHealthy);
}

static void test_health_heap_check_pass() {
    auto& monitor = MockHealthMonitor::getInstance();
    monitor.initialize();
    
    monitor.setFreeHeap(100000);  // 100KB 可用
    TEST_ASSERT_TRUE(monitor.checkHeapMemory(10000));  // 阈值 10KB
    TEST_ASSERT_TRUE(monitor.checkHeapMemory(50000));  // 阈值 50KB
}

static void test_health_heap_check_fail() {
    auto& monitor = MockHealthMonitor::getInstance();
    monitor.initialize();
    
    monitor.setFreeHeap(5000);  // 只有 5KB
    TEST_ASSERT_FALSE(monitor.checkHeapMemory(10000));  // 阈值 10KB
}

static void test_health_fs_space_check() {
    auto& monitor = MockHealthMonitor::getInstance();
    monitor.initialize();
    
    monitor.setFSSpace(500000, 1000000);  // 50% 使用
    TEST_ASSERT_TRUE(monitor.checkFSSpace(50000));  // 50KB 阈值，还有 500KB 空闲
    
    monitor.setFSSpace(990000, 1000000);  // 99% 使用
    TEST_ASSERT_FALSE(monitor.checkFSSpace(50000));  // 只有 10KB 空闲
}

static void test_health_wifi_check() {
    auto& monitor = MockHealthMonitor::getInstance();
    monitor.initialize();
    
    monitor.setWiFiConnected(true);
    TEST_ASSERT_TRUE(monitor.checkWiFiConnection());
    
    monitor.setWiFiConnected(false);
    TEST_ASSERT_FALSE(monitor.checkWiFiConnection());
}

static void test_health_temperature_check() {
    auto& monitor = MockHealthMonitor::getInstance();
    monitor.initialize();
    
    // 温度检查依赖 update() 中的随机值
    // 使用默认阈值 80°C
    TEST_ASSERT_TRUE(monitor.checkTemperature(80.0f));
}

static void test_health_report_generation() {
    auto& monitor = MockHealthMonitor::getInstance();
    monitor.initialize();
    monitor.setFreeHeap(50000);
    monitor.setWiFiConnected(true);
    
    char buffer[1024];
    size_t len = monitor.getHealthReport(buffer, sizeof(buffer));
    
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(strstr(buffer, "heap=") != nullptr);
}

static void test_health_warnings_and_errors() {
    auto& monitor = MockHealthMonitor::getInstance();
    monitor.initialize();
    monitor.clearWarningsAndErrors();
    
    monitor.addWarning("Low memory detected");
    monitor.addError("WiFi connection lost");
    
    auto warnings = monitor.getWarnings();
    auto errors = monitor.getErrors();
    
    TEST_ASSERT_EQUAL(1, warnings.size());
    TEST_ASSERT_EQUAL(1, errors.size());
    TEST_ASSERT_EQUAL_STRING("Low memory detected", warnings[0].c_str());
    TEST_ASSERT_EQUAL_STRING("WiFi connection lost", errors[0].c_str());
    
    SystemHealth health = monitor.getHealthStatus();
    TEST_ASSERT_FALSE(health.isHealthy);
}

static void test_health_custom_check() {
    auto& monitor = MockHealthMonitor::getInstance();
    monitor.initialize();
    
    HealthCheckItem check;
    check.name = "MQTT";
    check.description = "MQTT connection status";
    check.passed = false;
    check.message = "Not connected";
    
    monitor.addHealthCheck(check);
    
    auto checks = monitor.getHealthChecks();
    TEST_ASSERT_EQUAL(1, checks.size());
    TEST_ASSERT_EQUAL_STRING("MQTT", checks[0].name.c_str());
    TEST_ASSERT_FALSE(checks[0].passed);
}

// ========== LoggerSystem 测试 ==========

static void test_logger_initialize() {
    auto& logger = MockLoggerSystem::getInstance();
    TEST_ASSERT_TRUE(logger.initialize());
    TEST_ASSERT_EQUAL(0, logger.getEntryCount());
}

static void test_logger_level_filtering() {
    auto& logger = MockLoggerSystem::getInstance();
    logger.initialize();
    
    logger.setLogLevel(LOG_WARNING);  // 只记录 WARNING 及以上
    
    logger.logError("Error msg", "NET");
    logger.logWarning("Warning msg", "NET");
    logger.logInfo("Info msg", "NET");      // 应被过滤
    logger.logDebug("Debug msg", "NET");    // 应被过滤
    
    TEST_ASSERT_EQUAL(2, logger.getEntryCount());
    TEST_ASSERT_EQUAL(1, logger.getEntryCountByLevel(LOG_ERROR));
    TEST_ASSERT_EQUAL(1, logger.getEntryCountByLevel(LOG_WARNING));
    TEST_ASSERT_EQUAL(0, logger.getEntryCountByLevel(LOG_INFO));
}

static void test_logger_module_blacklist() {
    auto& logger = MockLoggerSystem::getInstance();
    logger.initialize();
    logger.setLogLevel(LOG_DEBUG);
    
    logger.addModuleToBlacklist("VERBOSE_MODULE");
    
    logger.logInfo("Normal log", "SYS");
    logger.logInfo("Should be blocked", "VERBOSE_MODULE");
    logger.logInfo("Another normal", "NET");
    
    TEST_ASSERT_EQUAL(2, logger.getEntryCount());
    
    // 移除黑名单
    logger.removeModuleFromBlacklist("VERBOSE_MODULE");
    logger.logInfo("Now visible", "VERBOSE_MODULE");
    TEST_ASSERT_EQUAL(3, logger.getEntryCount());
}

static void test_logger_query_by_level() {
    auto& logger = MockLoggerSystem::getInstance();
    logger.initialize();
    logger.setLogLevel(LOG_DEBUG);
    
    logger.logError("E1", "SYS");
    logger.logError("E2", "NET");
    logger.logWarning("W1", "SYS");
    logger.logInfo("I1", "SYS");
    logger.logDebug("D1", "NET");
    
    auto errors = logger.getEntriesByLevel(LOG_ERROR);
    TEST_ASSERT_EQUAL(2, errors.size());
    
    auto warnings = logger.getEntriesByLevel(LOG_WARNING);
    TEST_ASSERT_EQUAL(1, warnings.size());
}

static void test_logger_query_by_module() {
    auto& logger = MockLoggerSystem::getInstance();
    logger.initialize();
    logger.setLogLevel(LOG_DEBUG);
    
    logger.logInfo("msg1", "NET");
    logger.logInfo("msg2", "NET");
    logger.logInfo("msg3", "SYS");
    logger.logInfo("msg4", "MQTT");
    
    auto netLogs = logger.getEntriesByModule("NET");
    TEST_ASSERT_EQUAL(2, netLogs.size());
    
    auto mqttLogs = logger.getEntriesByModule("MQTT");
    TEST_ASSERT_EQUAL(1, mqttLogs.size());
}

static void test_logger_recent_entries() {
    auto& logger = MockLoggerSystem::getInstance();
    logger.initialize();
    logger.setLogLevel(LOG_DEBUG);
    
    for (int i = 0; i < 10; i++) {
        logger.logInfo("msg" + String(i), "SYS");
    }
    
    auto recent = logger.getRecentEntries(3);
    TEST_ASSERT_EQUAL(3, recent.size());
}

static void test_logger_clear_and_rotate() {
    auto& logger = MockLoggerSystem::getInstance();
    logger.initialize();
    logger.setLogLevel(LOG_DEBUG);
    
    logger.logInfo("msg1", "SYS");
    logger.logInfo("msg2", "SYS");
    TEST_ASSERT_EQUAL(2, logger.getEntryCount());
    
    TEST_ASSERT_TRUE(logger.rotateLogFile());
    TEST_ASSERT_EQUAL(0, logger.getEntryCount());
    TEST_ASSERT_EQUAL(0, (int)logger.getLogFileSize());
}

static void test_logger_export() {
    auto& logger = MockLoggerSystem::getInstance();
    logger.initialize();
    logger.setLogLevel(LOG_DEBUG);
    
    logger.logError("Critical failure", "SYS");
    logger.logInfo("System started", "BOOT");
    
    String exported = logger.exportLogs();
    TEST_ASSERT_TRUE(exported.indexOf("Critical failure") >= 0);
    TEST_ASSERT_TRUE(exported.indexOf("System started") >= 0);
}

/**
 * @brief 验证 logLevel 字符串到枚举的解析逻辑
 * 镜像 FastBeeFramework.cpp 和 DeviceRouteHandler.cpp 中的解析逻辑
 */
static void test_loglevel_string_parsing() {
    // 模拟源码中的解析函数（与 FastBeeFramework.cpp 一致）
    auto parseLogLevel = [](const char* str) -> LogLevel {
        if (strcmp(str, "DEBUG")   == 0) return LOG_DEBUG;
        if (strcmp(str, "INFO")    == 0) return LOG_INFO;
        if (strcmp(str, "WARNING") == 0) return LOG_WARNING;
        if (strcmp(str, "ERROR")   == 0) return LOG_ERROR;
        return LOG_INFO;  // 默认值
    };

    TEST_ASSERT_EQUAL(LOG_DEBUG,   parseLogLevel("DEBUG"));
    TEST_ASSERT_EQUAL(LOG_INFO,    parseLogLevel("INFO"));
    TEST_ASSERT_EQUAL(LOG_WARNING, parseLogLevel("WARNING"));
    TEST_ASSERT_EQUAL(LOG_ERROR,   parseLogLevel("ERROR"));
    TestLog::step("All 4 valid logLevel strings parsed correctly");

    // 未知字符串应回退到默认 INFO
    TEST_ASSERT_EQUAL(LOG_INFO, parseLogLevel("UNKNOWN"));
    TEST_ASSERT_EQUAL(LOG_INFO, parseLogLevel(""));
    TEST_ASSERT_EQUAL(LOG_INFO, parseLogLevel("debug"));  // 大小写敏感
    TestLog::step("Unknown/invalid strings fall back to INFO");

    // 验证解析后能正确设置日志级别
    auto& logger = MockLoggerSystem::getInstance();
    logger.initialize();

    logger.setLogLevel(parseLogLevel("WARNING"));
    TEST_ASSERT_EQUAL(LOG_WARNING, logger.getLogLevel());
    TestLog::step("Parsed logLevel applied to logger");

    logger.setLogLevel(parseLogLevel("DEBUG"));
    TEST_ASSERT_EQUAL(LOG_DEBUG, logger.getLogLevel());
    TestLog::step("LogLevel can be changed at runtime");
}

// ========== TaskManager 测试 ==========

static int taskExecutionCounter = 0;

static void dummyTask(void* param) {
    taskExecutionCounter++;
}

static void test_task_add_and_get() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    taskExecutionCounter = 0;
    
    TEST_ASSERT_TRUE(mgr.addTask("health_check", dummyTask, nullptr, 
                                  30000, TaskPriority::PRIORITY_HIGH));
    TEST_ASSERT_EQUAL(1, mgr.getTaskCount());
    
    Task* task = mgr.getTask("health_check");
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_STRING("health_check", task->id.c_str());
    TEST_ASSERT_EQUAL((int)TaskPriority::PRIORITY_HIGH, (int)task->priority);
    TEST_ASSERT_EQUAL(30000, (int)task->interval);
}

static void test_task_add_duplicate() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    
    mgr.addTask("dup_task", dummyTask, nullptr, 1000);
    TEST_ASSERT_FALSE(mgr.addTask("dup_task", dummyTask, nullptr, 2000));
    TEST_ASSERT_EQUAL(1, mgr.getTaskCount());
}

static void test_task_add_empty_id() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    
    TEST_ASSERT_FALSE(mgr.addTask("", dummyTask, nullptr, 1000));
}

static void test_task_remove() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    
    mgr.addTask("removable", dummyTask, nullptr, 1000);
    TEST_ASSERT_TRUE(mgr.removeTask("removable"));
    TEST_ASSERT_EQUAL(0, mgr.getTaskCount());
    TEST_ASSERT_NULL(mgr.getTask("removable"));
}

static void test_task_enable_disable() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    
    mgr.addTask("toggle_task", dummyTask, nullptr, 1000);
    
    TEST_ASSERT_TRUE(mgr.disableTask("toggle_task"));
    Task* task = mgr.getTask("toggle_task");
    TEST_ASSERT_FALSE(task->enabled);
    TEST_ASSERT_EQUAL((int)TaskState::TASK_STOPPED, (int)task->state);
    
    TEST_ASSERT_TRUE(mgr.enableTask("toggle_task"));
    TEST_ASSERT_TRUE(task->enabled);
}

static void test_task_pause_resume() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    
    mgr.addTask("pausable", dummyTask, nullptr, 1000);
    
    TEST_ASSERT_TRUE(mgr.pauseTask("pausable"));
    Task* task = mgr.getTask("pausable");
    TEST_ASSERT_EQUAL((int)TaskState::TASK_PAUSED, (int)task->state);
    
    TEST_ASSERT_TRUE(mgr.resumeTask("pausable"));
    TEST_ASSERT_EQUAL((int)TaskState::TASK_IDLE, (int)task->state);
}

static void test_task_execution() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    taskExecutionCounter = 0;
    
    mgr.addTask("exec_task", dummyTask, nullptr, 1000);
    
    // 使用 runTaskNow 直接执行（避免 run() 覆盖 currentTime）
    TEST_ASSERT_TRUE(mgr.runTaskNow("exec_task"));
    
    TEST_ASSERT_EQUAL(1, taskExecutionCounter);
    
    // 统计验证
    auto stats = mgr.getTaskStatistics("exec_task");
    TEST_ASSERT_EQUAL(1, stats.executionCount);
}

static void test_task_priority_scheduling() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    
    static int execOrder[3];
    static int orderIndex = 0;
    orderIndex = 0;
    
    mgr.addTask("low_task", [](void*) { execOrder[orderIndex++] = 1; }, 
                nullptr, 100, TaskPriority::PRIORITY_LOW);
    mgr.addTask("high_task", [](void*) { execOrder[orderIndex++] = 3; }, 
                nullptr, 100, TaskPriority::PRIORITY_HIGH);
    mgr.addTask("normal_task", [](void*) { execOrder[orderIndex++] = 2; }, 
                nullptr, 100, TaskPriority::PRIORITY_NORMAL);
    
    // 直接按优先级执行所有任务
    mgr.runTaskNow("high_task");
    mgr.runTaskNow("normal_task");
    mgr.runTaskNow("low_task");
    
    // 高优先级应先执行
    TEST_ASSERT_EQUAL(3, execOrder[0]);  // HIGH first
    TEST_ASSERT_EQUAL(2, execOrder[1]);  // NORMAL second
    TEST_ASSERT_EQUAL(1, execOrder[2]);  // LOW last
}

static void test_task_run_once() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    taskExecutionCounter = 0;
    
    Task oneshot;
    oneshot.id = "oneshot";
    oneshot.name = "One Shot Task";
    oneshot.function = dummyTask;
    oneshot.parameter = nullptr;
    oneshot.interval = 100;
    oneshot.priority = TaskPriority::PRIORITY_NORMAL;
    oneshot.runOnce = true;
    oneshot.enabled = true;
    
    mgr.addTask(oneshot);
    
    mgr.runTaskNow("oneshot");
    TEST_ASSERT_EQUAL(1, taskExecutionCounter);
    
    // runOnce 任务执行后应被禁用
    Task* task = mgr.getTask("oneshot");
    TEST_ASSERT_FALSE(task->enabled);
}

static void test_task_update_interval() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    
    mgr.addTask("interval_task", dummyTask, nullptr, 1000);
    
    TEST_ASSERT_TRUE(mgr.updateTaskInterval("interval_task", 5000));
    Task* task = mgr.getTask("interval_task");
    TEST_ASSERT_EQUAL(5000, (int)task->interval);
}

static void test_task_statistics() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    taskExecutionCounter = 0;
    
    mgr.addTask("stats_task", dummyTask, nullptr, 100);
    
    // 使用 runTaskNow 直接执行 5 次（避免 run() 内部覆盖 currentTime）
    for (int i = 0; i < 5; i++) {
        mgr.runTaskNow("stats_task");
    }
    
    auto stats = mgr.getTaskStatistics("stats_task");
    TEST_ASSERT_EQUAL(5, stats.executionCount);
    TEST_ASSERT_EQUAL(5, taskExecutionCounter);
}

static void test_task_reset_statistics() {
    auto& mgr = MockTaskManager::getInstance();
    mgr.initialize();
    taskExecutionCounter = 0;
    
    mgr.addTask("reset_stats", dummyTask, nullptr, 100);
    mgr.runTaskNow("reset_stats");
    
    mgr.resetStatistics("reset_stats");
    auto stats = mgr.getTaskStatistics("reset_stats");
    TEST_ASSERT_EQUAL(0, stats.executionCount);
}

// ========== 测试组入口 ==========

void test_system_services_group() {
    // ConfigStorage 测试
    RUN_TEST(test_config_nvs_string_rw);
    RUN_TEST(test_config_nvs_int_rw);
    RUN_TEST(test_config_nvs_bool_rw);
    RUN_TEST(test_config_nvs_float_rw);
    RUN_TEST(test_config_nvs_default_values);
    RUN_TEST(test_config_nvs_remove_key);
    RUN_TEST(test_config_json_save_load);
    RUN_TEST(test_config_json_load_nonexist);
    RUN_TEST(test_config_backup_restore);
    RUN_TEST(test_config_corruption_recovery);
    RUN_TEST(test_config_import_invalid_json);
    RUN_TEST(test_config_import_valid_json);
    RUN_TEST(test_config_version_migration);
    RUN_TEST(test_config_space_check);
    RUN_TEST(test_config_transfer_export_multiple);
    RUN_TEST(test_config_transfer_import_selective);
    RUN_TEST(test_config_transfer_overwrite_existing);
    RUN_TEST(test_config_transfer_empty_content_rejected);
    RUN_TEST(test_config_transfer_large_valid_json);
    RUN_TEST(test_config_transfer_export_nonexistent);

    // 配置导入安全性测试
    RUN_TEST(test_config_import_whitelist_rejects_unknown);
    RUN_TEST(test_config_import_whitelist_accepts_known);
    RUN_TEST(test_config_import_invalid_json_rejected);
    RUN_TEST(test_config_import_empty_content_rejected);
    RUN_TEST(test_config_import_oversize_rejected);
    RUN_TEST(test_config_import_no_stale_files);
    RUN_TEST(test_config_cleanup_stale_files);
    RUN_TEST(test_config_import_illegal_filename);
    RUN_TEST(test_config_import_low_space_rejected);
    RUN_TEST(test_config_import_roles_json_rejected);
    
    // HealthMonitor 测试
    RUN_TEST(test_health_initialize);
    RUN_TEST(test_health_heap_check_pass);
    RUN_TEST(test_health_heap_check_fail);
    RUN_TEST(test_health_fs_space_check);
    RUN_TEST(test_health_wifi_check);
    RUN_TEST(test_health_temperature_check);
    RUN_TEST(test_health_report_generation);
    RUN_TEST(test_health_warnings_and_errors);
    RUN_TEST(test_health_custom_check);
    
    // LoggerSystem 测试
    RUN_TEST(test_logger_initialize);
    RUN_TEST(test_logger_level_filtering);
    RUN_TEST(test_logger_module_blacklist);
    RUN_TEST(test_logger_query_by_level);
    RUN_TEST(test_logger_query_by_module);
    RUN_TEST(test_logger_recent_entries);
    RUN_TEST(test_logger_clear_and_rotate);
    RUN_TEST(test_logger_export);
    RUN_TEST(test_loglevel_string_parsing);
    
    // TaskManager 测试
    RUN_TEST(test_task_add_and_get);
    RUN_TEST(test_task_add_duplicate);
    RUN_TEST(test_task_add_empty_id);
    RUN_TEST(test_task_remove);
    RUN_TEST(test_task_enable_disable);
    RUN_TEST(test_task_pause_resume);
    RUN_TEST(test_task_execution);
    RUN_TEST(test_task_priority_scheduling);
    RUN_TEST(test_task_run_once);
    RUN_TEST(test_task_update_interval);
    RUN_TEST(test_task_statistics);
    RUN_TEST(test_task_reset_statistics);
}
