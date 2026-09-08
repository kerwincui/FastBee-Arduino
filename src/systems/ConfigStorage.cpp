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
#include "core/FeatureFlags.h"
#include <LittleFS.h>
#include <core/SystemConstants.h>

namespace {
bool verifyJsonFile(const String& path, JsonDocument* out = nullptr) {
    File file = LittleFS.open(path, FILE_READ);
    if (!file) return false;
    JsonDocument parsed;
    DeserializationError error = deserializeJson(parsed, file);
    file.close();
    if (error) return false;
    if (out) *out = parsed;
    return true;
}

// Commit a verified temporary file while retaining the previous generation.
// If power is lost after staging the primary, the next load uses .bak.
bool atomicReplaceFromTmp(const String& tmpPath, const String& filename) {
    if (!LittleFS.exists(tmpPath) || !verifyJsonFile(tmpPath)) return false;

    const String backupPath = filename + ".bak";
    const bool hadPrimary = LittleFS.exists(filename);
    LittleFS.remove(backupPath);
    if (hadPrimary && !LittleFS.rename(filename, backupPath)) {
        LittleFS.remove(tmpPath);
        return false;
    }
    if (LittleFS.rename(tmpPath, filename)) return true;

    if (hadPrimary) LittleFS.rename(backupPath, filename);
    LittleFS.remove(tmpPath);
    return false;
}
}  // namespace

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

    // Do not auto-format here. A transient mount failure or damaged metadata
    // must not erase the primary and backup configuration generations.
    if (!LittleFS.begin(false)) {
        LOG_ERROR("ConfigStorage: Failed to mount LittleFS; explicit recovery required");
        return false;
    }

    // 确保必要目录存在（首次启动或文件系统格式化后自动创建）
    const char* requiredDirs[] = {
        "/config",
#if FASTBEE_ENABLE_FILE_LOGGING || FASTBEE_ENABLE_LOG_VIEWER
        "/logs",
#endif
        "/www"
    };
    for (const char* dir : requiredDirs) {
        if (!LittleFS.exists(dir)) {
            LittleFS.mkdir(dir);
        }
    }

#if FASTBEE_ENABLE_STORAGE_CACHE
    if (!_cacheMutex) {
        _cacheMutex = xSemaphoreCreateMutex();
    }
#endif

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
    if (!_initialized && !initializeInternal()) return false;
#if FASTBEE_ENABLE_STORAGE_CACHE
    // 判断是否为关键配置（需立即写入）
    bool isCritical = (filename.indexOf("protocol") >= 0 || filename.indexOf("network") >= 0 ||
                       filename.indexOf("users") >= 0 || filename.indexOf("auth") >= 0 ||
                       filename.indexOf("features") >= 0);

    {
        CacheLock lock(_cacheMutex);
        if (lock) {
            // 更新缓存：返回 false 表示文件超阈未缓存，需直接写磁盘
            bool cached = updateCacheEntry(filename, config, 0);  // modTime=0 表示待写入

            if (cached && !isCritical) {
                // 非关键配置：标记为 dirty，稍后通过定时任务写入
                ConfigCacheEntry* entry = findInCache(filename);
                if (entry) {
                    entry->dirty = true;
                    entry->debounceUntil = millis() + 3000;
                }
                return true;  // 延迟写入
            }
            // 大文件或关键配置：继续执行立即写入
        }
    }

    // 关键配置或缓存超阈：立即写入（文件 I/O 不需要锁）
    bool result = flushToDisk(filename, config);
    if (result) {
        CacheLock lock(_cacheMutex);
        if (lock) {
            ConfigCacheEntry* entry = findInCache(filename);
            if (entry) {
                entry->dirty = false;
                entry->rawJson = "";
                entry->lastLoadTime = 0;
            }
        }
    }
    return result;
#else
    // 未启用缓存：原子写入（临时文件 + rename 覆盖）
    String tmpPath = filename + ".tmp";
    File file = LittleFS.open(tmpPath, FILE_WRITE);
    if (!file) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ConfigStorage: Cannot open %s for write", tmpPath.c_str());
        LOG_ERROR(buf);
        return false;
    }

    const size_t expected = measureJson(config);
    size_t written = serializeJson(config, file);
    file.flush();
    file.close();
    if (written != expected || !verifyJsonFile(tmpPath)) {
        LittleFS.remove(tmpPath);
        return false;
    }
    return atomicReplaceFromTmp(tmpPath, filename);
#endif
}

bool ConfigStorage::loadJSONConfig(const String& filename, JsonDocument& config) {
    if (!_initialized && !initializeInternal()) return false;
#if FASTBEE_ENABLE_STORAGE_CACHE
    // 1. 查询缓存（加锁）
    {
        CacheLock lock(_cacheMutex);
        if (lock) {
            ConfigCacheEntry* entry = findInCache(filename);
            if (entry && entry->rawJson.length() > 0 && entry->lastLoadTime > 0) {
                // 读己之写：dirty 条目（debounce 待写入）本身就是最新数据，
                // 磁盘上还是旧版本，跳过 mtime 检查直接命中；否则会读回旧数据
                // 并在 updateCacheEntry 中用旧数据覆盖待写入的新配置（写丢失）
                bool cacheFresh = entry->dirty;
                if (!cacheFresh) {
                    // 检查文件是否被外部修改
                    File checkFile = LittleFS.open(filename, FILE_READ);
                    if (checkFile) {
                        time_t fileTime = checkFile.getLastWrite();
                        checkFile.close();
                        cacheFresh = (fileTime <= entry->fileModifyTime);
                    }
                }
                if (cacheFresh) {
                    // 缓存命中：从 rawJson 反序列化到调用方 Doc
                    // （相比旧实现 config = *entry->cachedDoc 的 DOM 深拷贝，
                    //   绕行缓存是小文件 <=4KB，临时峰值几乎可忽）
                    DeserializationError err = deserializeJson(config, entry->rawJson);
                    if (!err) {
                        entry->accessCount++;
                        return true;
                    }
                    // 缓存文本损坏，清除后走文件 I/O
                    entry->rawJson = "";
                    entry->lastLoadTime = 0;
                    entry->dirty = false;
                }
            }
        }
    }
#endif

    // 文件 I/O（不需要锁）。主槽损坏或缺失时回退到上一代备份。
    String loadPath = filename;
    bool recoveredFromBackup = false;
    if (!verifyJsonFile(loadPath, &config)) {
        loadPath = filename + ".bak";
        config.clear();
        if (!verifyJsonFile(loadPath, &config)) {
            LOG_ERRORF("ConfigStorage: No valid primary or backup JSON for %s", filename.c_str());
            return false;
        }
        recoveredFromBackup = true;
        LOG_WARNINGF("ConfigStorage: Recovered %s from backup slot", filename.c_str());
    }

    File metadataFile = LittleFS.open(loadPath, FILE_READ);
    time_t modTime = metadataFile ? metadataFile.getLastWrite() : 0;
    if (metadataFile) metadataFile.close();

    // Restore the recovered generation as primary so the next save cannot
    // rotate a corrupt primary over the last known-good backup.
    if (recoveredFromBackup) {
        LittleFS.remove(filename);
        if (!LittleFS.rename(loadPath, filename)) {
            LOG_ERRORF("ConfigStorage: Failed to restore recovered primary %s", filename.c_str());
        }
    }

#if FASTBEE_ENABLE_STORAGE_CACHE
    // 更新缓存（加锁）；大文件会被 updateCacheEntry 内部跳过
    {
        CacheLock lock(_cacheMutex);
        if (lock) {
            updateCacheEntry(filename, config, modTime);
        }
    }
#endif

    return true;
}

// ── 特定配置快捷接口 ─────────────────────────────────────────────────────────

bool ConfigStorage::saveNetworkConfig(const JsonDocument& config)   { return saveJSONConfig(FileSystem::NETWORK_CONFIG_FILE,   config); }
bool ConfigStorage::loadNetworkConfig(JsonDocument& config)         { return loadJSONConfig(FileSystem::NETWORK_CONFIG_FILE,   config); }
bool ConfigStorage::saveUserConfig(const JsonDocument& config)      { return saveJSONConfig(FileSystem::USER_CONFIG_FILE,      config); }
bool ConfigStorage::loadUserConfig(JsonDocument& config)            { return loadJSONConfig(FileSystem::USER_CONFIG_FILE,      config); }
bool ConfigStorage::saveProtocolConfig(const JsonDocument& config)  { return saveJSONConfig(FileSystem::PROTOCOL_CONFIG_FILE,  config); }
bool ConfigStorage::loadProtocolConfig(JsonDocument& config)        { return loadJSONConfig(FileSystem::PROTOCOL_CONFIG_FILE,  config); }

// 使用 ArduinoJson Filter 仅反序列化 protocol.json 的单个顶层子节点
// 优势：堆峰值从 ~15KB 降至对应子节点实际体积（如 mqtt ~3KB, coap/tcp/http <0.5KB）
// 注意：本方法绕过 StorageCache（protocol.json > 4KB 阈值），直接走 LittleFS
bool ConfigStorage::loadProtocolSection(const String& section, JsonDocument& outDoc) {
    return loadConfigSection(FileSystem::PROTOCOL_CONFIG_FILE, section, outDoc);
}

// 通用分段加载：从任意 JSON 文件中仅反序列化指定顶层 section
// 使用 ArduinoJson Filter 机制，堆峰值仅为对应子节点实际体积，而非整文件
bool ConfigStorage::loadConfigSection(const char* filename, const String& section, JsonDocument& outDoc) {
    if (!_initialized && !initializeInternal()) {
        return false;
    }

    // 构造 Filter：{ "<section>": true }，只保留匹配子节点，其它全部跳过
    JsonDocument filter;
    filter[section] = true;

    auto loadSectionFrom = [&](const String& path) -> bool {
        File file = LittleFS.open(path, FILE_READ);
        if (!file) return false;
        outDoc.clear();
        DeserializationError error = deserializeJson(outDoc, file, DeserializationOption::Filter(filter));
        file.close();
        return !error && outDoc[section].is<JsonObject>();
    };

    const String primaryPath(filename);
    if (loadSectionFrom(primaryPath)) return true;

    const String backupPath = primaryPath + ".bak";
    if (!loadSectionFrom(backupPath)) {
        LOG_ERRORF("ConfigStorage: No valid '%s' section in primary or backup %s",
                   section.c_str(), filename);
        return false;
    }

    LOG_WARNINGF("ConfigStorage: Recovered section '%s' from backup %s",
                 section.c_str(), filename);
    LittleFS.remove(primaryPath);
    if (!LittleFS.rename(backupPath, primaryPath)) {
        LOG_ERRORF("ConfigStorage: Failed to restore recovered primary %s", filename);
    }
    return true;
}

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
#if FASTBEE_ENABLE_STORAGE_CACHE
    // 删除文件时也清除对应缓存（不 flush）
    {
        CacheLock lock(_cacheMutex);
        if (lock) {
            ConfigCacheEntry* entry = findInCache(filename);
            if (entry) {
                entry->dirty = false;  // 不需要 flush，文件将被删除
                size_t idx = entry - _cache;
                if (idx < _cacheSize - 1) {
                    _cache[idx] = std::move(_cache[_cacheSize - 1]);
                }
                _cacheSize--;
            }
        }
    }
#endif
    return LittleFS.remove(filename);
}

// ── 缓存辅助方法 ──────────────────────────────────────────────────────────────

#if FASTBEE_ENABLE_STORAGE_CACHE

ConfigStorage::ConfigCacheEntry* ConfigStorage::findInCache(const String& filename) {
    for (size_t i = 0; i < _cacheSize; i++) {
        if (_cache[i].filename == filename) {
            return &_cache[i];
        }
    }
    return nullptr;
}

bool ConfigStorage::evictLRUEntry() {
    if (_cacheSize == 0) return false;

    // 找到访问次数最少的非 dirty 条目
    size_t minIdx = 0;
    uint8_t minCount = 255;
    bool foundClean = false;
    for (size_t i = 0; i < _cacheSize; i++) {
        if (!_cache[i].dirty && _cache[i].accessCount < minCount) {
            minCount = _cache[i].accessCount;
            minIdx = i;
            foundClean = true;
        }
    }

    // 如果没有干净的条目，找访问次数最少的 dirty 条目
    if (!foundClean) {
        minCount = 255;
        for (size_t i = 0; i < _cacheSize; i++) {
            if (_cache[i].accessCount < minCount) {
                minCount = _cache[i].accessCount;
                minIdx = i;
            }
        }
    }

    // 如果找到的条目是 dirty 的，先 flush（直接用 rawJson，避免重序列化）
    if (_cache[minIdx].dirty && _cache[minIdx].rawJson.length() > 0 &&
        !flushRawToDisk(_cache[minIdx].filename, _cache[minIdx].rawJson)) {
        LOG_ERRORF("ConfigStorage: Refusing to evict unsaved cache entry %s",
                   _cache[minIdx].filename.c_str());
        return false;
    }

    // 移除：将最后一个条目移到被淘汰的位置
    if (minIdx < _cacheSize - 1) {
        _cache[minIdx] = std::move(_cache[_cacheSize - 1]);
    }
    _cacheSize--;
    return true;
}

bool ConfigStorage::updateCacheEntry(const String& filename, const JsonDocument& doc, time_t modTime) {
    // 先估测序列化后的大小，超阈值直接不缓存。
    // measureJson 只遍历 DOM 不分配新内存，开销很小。
    const size_t estSize = measureJson(doc);
    if (estSize > CACHE_MAX_FILE_BYTES) {
        // 大文件绕过缓存。不要在这里淘汰同名 dirty 条目；调用方只有在
        // 新内容成功直写磁盘后才能使旧缓存失效。
        return false;
    }

    String serialized;
    serialized.reserve(estSize + 16);
    const size_t written = serializeJson(doc, serialized);
    if (written != estSize) return false;

    ConfigCacheEntry* entry = findInCache(filename);
    if (entry && entry->dirty && modTime != 0) {
        // load 回填路径（modTime!=0）不得用磁盘旧数据覆盖 debounce 待写入的
        // 新配置，否则后续 flush 会把旧数据写回磁盘造成写丢失
        return true;
    }
    if (!entry) {
        // 缓存已满，淘汰一个
        if (_cacheSize >= MAX_CONFIG_CACHE_ENTRIES) {
            if (!evictLRUEntry()) return false;
        }
        entry = &_cache[_cacheSize++];
        entry->filename = filename;
        entry->accessCount = 0;
    }
    // Only replace the cached generation after serialization succeeded.
    entry->rawJson = std::move(serialized);
    entry->lastLoadTime = millis();
    entry->fileModifyTime = modTime;
    entry->accessCount++;
    return true;
}

bool ConfigStorage::flushToDisk(const String& filename, const JsonDocument& config) {
    // 原子写：先写临时文件再 rename 覆盖，断电时保留旧版本而非半截文件
    String tmpPath = filename + ".tmp";
    File file = LittleFS.open(tmpPath, FILE_WRITE);
    if (!file) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ConfigStorage: Cannot open %s for write", tmpPath.c_str());
        LOG_ERROR(buf);
        return false;
    }
    const size_t expected = measureJson(config);
    size_t written = serializeJson(config, file);
    file.flush();
    file.close();
    if (written != expected || !verifyJsonFile(tmpPath)) {
        LittleFS.remove(tmpPath);
        return false;
    }
    return atomicReplaceFromTmp(tmpPath, filename);
}

bool ConfigStorage::flushRawToDisk(const String& filename, const String& rawJson) {
    String tmpPath = filename + ".tmp";
    File file = LittleFS.open(tmpPath, FILE_WRITE);
    if (!file) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ConfigStorage: Cannot open %s for write", tmpPath.c_str());
        LOG_ERROR(buf);
        return false;
    }
    size_t written = file.print(rawJson);
    file.close();
    if (written != rawJson.length() || !verifyJsonFile(tmpPath)) {
        LittleFS.remove(tmpPath);
        return false;
    }
    return atomicReplaceFromTmp(tmpPath, filename);
}

void ConfigStorage::flushDirtyEntries(bool force) {
    CacheLock lock(_cacheMutex);
    if (!lock) return;

    unsigned long now = millis();
    for (size_t i = 0; i < _cacheSize; i++) {
        // 有符号差值比较兼容 millis 回绕（debounceUntil = millis()+3000 可能溢出）
        if (_cache[i].dirty && (force || (long)(now - _cache[i].debounceUntil) >= 0) &&
            _cache[i].rawJson.length() > 0) {
            if (flushRawToDisk(_cache[i].filename, _cache[i].rawJson)) {
                _cache[i].dirty = false;
                _cache[i].fileModifyTime = 0; // 下次 load 时会刷新
            }
        }
    }
}

void ConfigStorage::clearCache() {
    CacheLock lock(_cacheMutex);
    if (!lock) return;

    size_t retained = 0;
    for (size_t i = 0; i < _cacheSize; i++) {
        const bool flushFailed = _cache[i].dirty && _cache[i].rawJson.length() > 0 &&
                                 !flushRawToDisk(_cache[i].filename, _cache[i].rawJson);
        if (flushFailed) {
            LOG_ERRORF("ConfigStorage: Retaining unsaved cache entry %s",
                       _cache[i].filename.c_str());
            if (retained != i) _cache[retained] = std::move(_cache[i]);
            retained++;
            continue;
        }
        _cache[i].rawJson = "";
        _cache[i].filename = "";
    }
    _cacheSize = retained;
}

void ConfigStorage::clearCache(const String& filename) {
    CacheLock lock(_cacheMutex);
    if (!lock) return;

    ConfigCacheEntry* entry = findInCache(filename);
    if (entry) {
        if (entry->dirty && entry->rawJson.length() > 0 &&
            !flushRawToDisk(entry->filename, entry->rawJson)) {
            LOG_ERRORF("ConfigStorage: Retaining unsaved cache entry %s", entry->filename.c_str());
            return;
        }
        // 移除条目
        size_t idx = entry - _cache;
        if (idx < _cacheSize - 1) {
            _cache[idx] = std::move(_cache[_cacheSize - 1]);
        }
        _cacheSize--;
    }
}

#endif // FASTBEE_ENABLE_STORAGE_CACHE
