/**
 * @description: 配置存储系统实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:32:23
 *
 * 优化说明：
 *  1. 单例由裸指针改为局部静态实例（Meyers' Singleton），线程安全且无内存泄漏
 *  2. 所有 Serial.println 替换为 LoggerSystem 调用
 *  3. isFileSystemOK() 不再重复 begin()，改为检测挂载状态
 *  4. formatFileSystem() / listConfigFiles() 使用 LOG 宏统一输出
 */

#include "systems/ConfigStorage.h"
#include "systems/LoggerSystem.h"
#include <LittleFS.h>
#include <core/SystemConstants.h>

// ── 单例（Meyers' Singleton，无裸指针，线程安全）────────────────────────────
ConfigStorage& ConfigStorage::getInstance() {
    static ConfigStorage instance;
    return instance;
}

bool ConfigStorage::isInitialized() {
    return getInstance()._initialized;
}

// ── 构造 / 内部初始化 ────────────────────────────────────────────────────────

ConfigStorage::ConfigStorage() = default;

bool ConfigStorage::initializeInternal() {
    if (_initialized) return true;

    if (!prefs.begin(Storage::PREFERENCES_NAMESPACE, false)) {
        LOG_ERROR("ConfigStorage: Failed to initialize Preferences");
        return false;
    }

    if (!LittleFS.begin(true)) {
        LOG_ERROR("ConfigStorage: Failed to initialize LittleFS");
        return false;
    }

    // 确保必要目录存在（首次启动或文件系统格式化后自动创建）
    const char* requiredDirs[] = { "/config", "/logs", "/www" };
    for (const char* dir : requiredDirs) {
        if (!LittleFS.exists(dir)) {
            LittleFS.mkdir(dir);
        }
    }

    _initialized = true;
    LOG_INFO("ConfigStorage: Initialized");
    return true;
}

// ── NVS 键值接口 ─────────────────────────────────────────────────────────────

bool ConfigStorage::saveString(const String& key, const String& value) {
    return prefs.putString(key.c_str(), value) > 0;
}

String ConfigStorage::readString(const String& key, const String& defaultValue) {
    return prefs.getString(key.c_str(), defaultValue);
}

bool ConfigStorage::saveInt(const String& key, int value) {
    return prefs.putInt(key.c_str(), value) > 0;
}

int ConfigStorage::readInt(const String& key, int defaultValue) {
    return prefs.getInt(key.c_str(), defaultValue);
}

bool ConfigStorage::saveBool(const String& key, bool value) {
    return prefs.putBool(key.c_str(), value) > 0;
}

bool ConfigStorage::readBool(const String& key, bool defaultValue) {
    return prefs.getBool(key.c_str(), defaultValue);
}

bool ConfigStorage::removeKey(const String& key) {
    return prefs.remove(key.c_str());
}

// ── JSON 文件接口 ─────────────────────────────────────────────────────────────

bool ConfigStorage::saveJSONConfig(const String& filename, const JsonDocument& config) {
    File file = LittleFS.open(filename, FILE_WRITE);
    if (!file) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ConfigStorage: Cannot open %s for write", filename.c_str());
        LOG_ERROR(buf);
        return false;
    }

    size_t written = serializeJson(config, file);
    file.close();
    return written > 0;
}

bool ConfigStorage::loadJSONConfig(const String& filename, JsonDocument& config) {
    File file = LittleFS.open(filename, FILE_READ);
    if (!file) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ConfigStorage: Cannot open %s for read", filename.c_str());
        LOG_WARNING(buf);
        return false;
    }

    DeserializationError err = deserializeJson(config, file);
    file.close();

    if (err) {
        char buf[80];
        snprintf(buf, sizeof(buf), "ConfigStorage: JSON parse error in %s: %s",
                 filename.c_str(), err.c_str());
        LOG_ERROR(buf);
        return false;
    }
    return true;
}

// ── 特定配置快捷接口 ─────────────────────────────────────────────────────────

bool ConfigStorage::saveNetworkConfig(const JsonDocument& config)   { return saveJSONConfig(FileSystem::NETWORK_CONFIG_FILE,   config); }
bool ConfigStorage::loadNetworkConfig(JsonDocument& config)         { return loadJSONConfig(FileSystem::NETWORK_CONFIG_FILE,   config); }
bool ConfigStorage::saveUserConfig(const JsonDocument& config)      { return saveJSONConfig(FileSystem::USER_CONFIG_FILE,      config); }
bool ConfigStorage::loadUserConfig(JsonDocument& config)            { return loadJSONConfig(FileSystem::USER_CONFIG_FILE,      config); }
bool ConfigStorage::saveProtocolConfig(const JsonDocument& config)  { return saveJSONConfig(FileSystem::PROTOCOL_CONFIG_FILE,  config); }
bool ConfigStorage::loadProtocolConfig(JsonDocument& config)        { return loadJSONConfig(FileSystem::PROTOCOL_CONFIG_FILE,  config); }
bool ConfigStorage::saveGpioConfig(const JsonDocument& config)      { return saveJSONConfig(FileSystem::GPIO_CONFIG_FILE,      config); }
bool ConfigStorage::loadGpioConfig(JsonDocument& config)            { return loadJSONConfig(FileSystem::GPIO_CONFIG_FILE,      config); }

// ── IConfigStorage 接口实现 ──────────────────────────────────────────────────

bool ConfigStorage::initialize() {
    return initializeInternal();
}

bool ConfigStorage::saveConfig(const String& key, const String& value) {
    return saveString(key, value);
}

String ConfigStorage::readConfig(const String& key, const String& defaultValue) {
    return readString(key, defaultValue);
}

bool ConfigStorage::deleteConfig(const String& key) {
    return removeKey(key);
}

bool ConfigStorage::hasConfig(const String& key) {
    return prefs.isKey(key.c_str());
}

// ── 文件系统管理 ─────────────────────────────────────────────────────────────

bool ConfigStorage::isFileSystemOK() {
    // 已挂载时 totalBytes() > 0
    return (LittleFS.totalBytes() > 0);
}

bool ConfigStorage::formatFileSystem() {
    LOG_WARNING("ConfigStorage: Formatting file system...");
    bool result = LittleFS.format();
    if (result) {
        LittleFS.begin(true);
        LOG_INFO("ConfigStorage: File system formatted");
    } else {
        LOG_ERROR("ConfigStorage: Format failed");
    }
    return result;
}

void ConfigStorage::listConfigFiles() {
    File root = LittleFS.open("/");
    if (!root) {
        LOG_ERROR("ConfigStorage: Cannot open root directory");
        return;
    }

    LOG_INFO("ConfigStorage: File list:");
    File file = root.openNextFile();
    while (file) {
        char buf[80];
        snprintf(buf, sizeof(buf), "  %s  (%u bytes)", file.name(), (unsigned)file.size());
        LOG_INFO(buf);
        file = root.openNextFile();
    }
}

bool ConfigStorage::deleteConfigFile(const String& filename) {
    return LittleFS.remove(filename);
}
