/**
 * @file MockConfigStorage.h
 * @brief 配置存储模拟对象
 * 
 * 提供配置持久化功能的模拟实现
 */

#ifndef MOCK_CONFIG_STORAGE_H
#define MOCK_CONFIG_STORAGE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <map>
#include <vector>

// 配置版本信息
struct ConfigVersion {
    int major;
    int minor;
    int patch;
    
    ConfigVersion(int maj = 1, int min = 0, int pat = 0)
        : major(maj), minor(min), patch(pat) {}
    
    String toString() const {
        return String(major) + "." + String(minor) + "." + String(patch);
    }
    
    bool operator<(const ConfigVersion& other) const {
        if (major != other.major) return major < other.major;
        if (minor != other.minor) return minor < other.minor;
        return patch < other.patch;
    }
};

// 模拟配置存储
class MockConfigStorage {
public:
    static MockConfigStorage& getInstance() {
        static MockConfigStorage instance;
        return instance;
    }

    bool initialize() {
        _initialized = true;
        _configVersion = ConfigVersion(1, 0, 0);
        return true;
    }

    // NVS操作（键值对）
    bool putString(const char* key, const String& value) {
        _nvsData[key] = value;
        return true;
    }

    bool putInt(const char* key, int value) {
        _nvsData[key] = String(value);
        return true;
    }

    bool putBool(const char* key, bool value) {
        _nvsData[key] = value ? "true" : "false";
        return true;
    }

    bool putFloat(const char* key, float value) {
        _nvsData[key] = String(value, 6);
        return true;
    }

    String getString(const char* key, const String& defaultValue = "") {
        auto it = _nvsData.find(key);
        if (it != _nvsData.end()) {
            return it->second;
        }
        return defaultValue;
    }

    int getInt(const char* key, int defaultValue = 0) {
        auto it = _nvsData.find(key);
        if (it != _nvsData.end()) {
            return it->second.toInt();
        }
        return defaultValue;
    }

    bool getBool(const char* key, bool defaultValue = false) {
        auto it = _nvsData.find(key);
        if (it != _nvsData.end()) {
            return it->second == "true";
        }
        return defaultValue;
    }

    float getFloat(const char* key, float defaultValue = 0.0f) {
        auto it = _nvsData.find(key);
        if (it != _nvsData.end()) {
            return it->second.toFloat();
        }
        return defaultValue;
    }

    bool removeKey(const char* key) {
        auto it = _nvsData.find(key);
        if (it != _nvsData.end()) {
            _nvsData.erase(it);
            return true;
        }
        return false;
    }

    bool exists(const char* key) {
        return _nvsData.find(key) != _nvsData.end();
    }

    std::vector<String> getAllKeys() {
        std::vector<String> keys;
        for (auto& entry : _nvsData) {
            keys.push_back(entry.first.c_str());
        }
        return keys;
    }

    // JSON配置文件操作
    bool saveConfig(const char* path, const JsonDocument& doc) {
        String jsonStr;
        serializeJson(doc, jsonStr);
        
        // 模拟原子写入
        String tempPath = String(path) + ".tmp";
        _jsonFiles[tempPath] = jsonStr;
        
        // 重命名（原子操作）
        _jsonFiles[path] = jsonStr;
        _jsonFiles.erase(tempPath);
        
        return true;
    }

    bool loadConfig(const char* path, JsonDocument& doc) {
        auto it = _jsonFiles.find(path);
        if (it == _jsonFiles.end()) {
            // 尝试加载备份
            String backupPath = String(path) + ".bak";
            it = _jsonFiles.find(backupPath);
            if (it == _jsonFiles.end()) {
                return false;
            }
        }
        
        DeserializationError error = deserializeJson(doc, it->second.c_str());
        return error == DeserializationError::Ok;
    }

    bool deleteConfig(const char* path) {
        auto it = _jsonFiles.find(path);
        if (it != _jsonFiles.end()) {
            _jsonFiles.erase(it);
            return true;
        }
        return false;
    }

    bool configExists(const char* path) {
        return _jsonFiles.find(path) != _jsonFiles.end();
    }

    // 配置备份和恢复
    bool backupConfig(const char* path, const char* backupPath) {
        auto it = _jsonFiles.find(path);
        if (it == _jsonFiles.end()) return false;
        
        _jsonFiles[backupPath] = it->second;
        return true;
    }

    bool restoreConfig(const char* backupPath, const char* path) {
        auto it = _jsonFiles.find(backupPath);
        if (it == _jsonFiles.end()) return false;
        
        _jsonFiles[path] = it->second;
        return true;
    }

    // 配置版本管理
    void setConfigVersion(const ConfigVersion& version) {
        _configVersion = version;
    }

    ConfigVersion getConfigVersion() {
        return _configVersion;
    }

    // 配置迁移
    bool migrateConfig(const ConfigVersion& fromVersion, 
                       const ConfigVersion& toVersion) {
        if (fromVersion < toVersion) {
            // 执行升级迁移
            _configVersion = toVersion;
            return true;
        }
        return false;
    }

    // 存储空间检查
    size_t getUsedSpace() {
        size_t total = 0;
        for (auto& entry : _nvsData) {
            total += entry.first.length() + entry.second.length();
        }
        for (auto& entry : _jsonFiles) {
            total += entry.first.length() + entry.second.length();
        }
        return total;
    }

    size_t getFreeSpace() {
        if (_lowSpaceMode) return 512;  // 低空间模式模拟不足
        return 100000 - getUsedSpace();  // 模拟100KB存储
    }

    // 配置导入白名单（与 SystemRouteHandler.cpp 保持一致）
    static inline const char* const kAllowedImportFiles[8] = {
        "device.json",
        "network.json",
        "peripherals.json",
        "periph_exec.json",
        "protocol.json",
        "users.json",
        "auth.json",
        "rule_scripts.json"
    };
    static constexpr int kAllowedImportFilesCount = 8;

    static bool isAllowedImportFileName(const String& name) {
        for (int i = 0; i < kAllowedImportFilesCount; ++i) {
            if (name == kAllowedImportFiles[i]) return true;
        }
        return false;
    }

    // 带完整验证的配置导入（模拟后端白名单+JSON验证+大小限制）
    String lastImportError;

    bool importConfigValidated(const String& name, const String& jsonData,
                               size_t maxSize = 24576) {
        lastImportError = "";

        // 1. 文件名安全检查
        if (name.isEmpty() || name.length() > 48) {
            lastImportError = "文件名过长或为空";
            return false;
        }
        if (name.indexOf('/') >= 0 || name.indexOf("\\\\") >= 0 || name.indexOf("..") >= 0) {
            lastImportError = "文件名包含非法字符";
            return false;
        }
        if (!name.endsWith(".json")) {
            lastImportError = "文件名必须以 .json 结尾";
            return false;
        }

        // 2. 白名单检查
        if (!isAllowedImportFileName(name)) {
            lastImportError = "不允许导入配置文件：" + name;
            return false;
        }

        // 3. 内容空检查
        if (jsonData.isEmpty()) {
            lastImportError = "配置文件内容为空";
            return false;
        }

        // 4. 大小限制
        if (jsonData.length() > maxSize) {
            lastImportError = "配置文件过大（" + String(jsonData.length()) + " 字节）";
            return false;
        }

        // 5. JSON 格式验证（必须为对象或数组，排除纯字符串/数字等标量）
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, jsonData.c_str());
        if (err != DeserializationError::Ok || !doc.is<JsonObject>() && !doc.is<JsonArray>()) {
            lastImportError = "配置文件内容不是有效的 JSON 格式";
            return false;
        }

        // 6. 存储空间检查
        if (getFreeSpace() < jsonData.length() * 2 + 1024) {
            lastImportError = "存储空间不足";
            return false;
        }

        String path = "/config/" + name;
        _jsonFiles[path] = jsonData;
        return true;
    }

    // 列举当前存储中的所有配置文件名
    std::vector<String> listConfigFiles() const {
        std::vector<String> names;
        for (auto& entry : _jsonFiles) {
            if (entry.first.startsWith("/config/")) {
                String name = entry.first.substring(8);  // 去除 /config/ 前缀
                names.push_back(name);
            }
        }
        return names;
    }

    size_t getConfigFileCount() const {
        size_t count = 0;
        for (auto& entry : _jsonFiles) {
            if (entry.first.startsWith("/config/")) count++;
        }
        return count;
    }

    // 清理不在白名单中的残留文件
    int cleanupStaleFiles() {
        int removed = 0;
        std::vector<String> toRemove;
        for (auto& entry : _jsonFiles) {
            if (entry.first.startsWith("/config/")) {
                String name = entry.first.substring(8);
                if (!isAllowedImportFileName(name)) {
                    toRemove.push_back(entry.first);
                }
            }
        }
        for (auto& path : toRemove) {
            _jsonFiles.erase(path);
            removed++;
        }
        return removed;
    }

    // 配置导出导入
    String exportConfig(const char* path) {
        auto it = _jsonFiles.find(path);
        if (it != _jsonFiles.end()) {
            return it->second;
        }
        return "";
    }

    bool importConfig(const char* path, const String& jsonData) {
        // 验证JSON格式
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, jsonData.c_str());
        if (error != DeserializationError::Ok) {
            return false;
        }
        
        _jsonFiles[path] = jsonData;
        return true;
    }

    // 批量操作
    void clearAll() {
        _nvsData.clear();
        _jsonFiles.clear();
    }

    void clearNVS() {
        _nvsData.clear();
    }

    void clearJSONFiles() {
        _jsonFiles.clear();
    }

    // 测试辅助方法
    void simulateCorruption(const char* path) {
        _jsonFiles[path] = "{invalid json";
    }

    void setLowSpaceMode(bool enabled) {
        _lowSpaceMode = enabled;
    }

    bool isLowSpace() {
        return _lowSpaceMode || getFreeSpace() < 10000;
    }

private:
    MockConfigStorage() : _initialized(false), _lowSpaceMode(false) {}

    bool _initialized;
    bool _lowSpaceMode;
    ConfigVersion _configVersion;
    std::map<String, String> _nvsData;
    std::map<String, String> _jsonFiles;
};

// 全局实例引用
#define MockConfigStore MockConfigStorage::getInstance()

#endif // MOCK_CONFIG_STORAGE_H
