/**
 * @file RuntimeFeatureManager.cpp
 * @brief 运行时功能配置管理器实现
 *
 * 注册所有 B/C/D 类功能（compiled 字段反映当前编译宏取值），
 * 用户配置持久化于 /config/features.json，驱动侧通过 shouldEnable() 门控。
 */

#include "core/RuntimeFeatureManager.h"

#include <string.h>
#include <time.h>

#include <LittleFS.h>

#include "core/FeatureBudget.h"
#include "core/FeatureFlags.h"
#include "systems/ConfigStorage.h"
#include "systems/LoggerSystem.h"

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace {
constexpr const char* kConfigPath = "/config/features.json";

struct FeatureBundle {
    const char* primary;
    const char* member;
};

// 保留旧键用于配置兼容，但每组只向用户展示 primary。
constexpr FeatureBundle kFeatureBundles[] = {
    {"ruleScript", "commandScript"},
    {"logViewer", "fileLogging"},
};
}  // namespace

namespace FastBee {

// ── 注册与查找 ─────────────────────────────────────────────────────────

void RuntimeFeatureManager::registerFeature(const FeatureMeta& meta) {
    if (count_ >= kMaxFeatures) return;
    FeatureState& st = features_[count_++];
    st.meta = meta;
    st.userEnabled = meta.compiled && meta.defaultEnabled;
    st.explicitlyConfigured = false;
    st.budgetPassed = true;
    st.runtimeDisabled = false;
    st.runtimeReason[0] = '\0';
    runtimeDegraded_ = false;
}

RuntimeFeatureManager::FeatureState* RuntimeFeatureManager::findFeature(const char* key) {
    if (!key) return nullptr;
    for (size_t i = 0; i < count_; ++i) {
        if (strcmp(features_[i].meta.key, key) == 0) return &features_[i];
    }
    return nullptr;
}

void RuntimeFeatureManager::setRuntimeDisabled(const char* key, const char* reason) {
    auto* feature = findFeature(key);
    if (!feature) return;
    feature->runtimeDisabled = true;
    feature->runtimeReason[0] = '\0';
    if (reason && reason[0] != '\0') {
        strncpy(feature->runtimeReason, reason, sizeof(feature->runtimeReason) - 1);
        feature->runtimeReason[sizeof(feature->runtimeReason) - 1] = '\0';
    }
    runtimeDegraded_ = true;
}

void RuntimeFeatureManager::clearRuntimeDisabled(const char* key) {
    auto* feature = findFeature(key);
    if (!feature) return;
    feature->runtimeDisabled = false;
    feature->runtimeReason[0] = '\0';
    runtimeDegraded_ = false;
    for (size_t i = 0; i < count_; ++i) {
        if (features_[i].runtimeDisabled) {
            runtimeDegraded_ = true;
            break;
        }
    }
}

bool RuntimeFeatureManager::isRuntimeDisabled(const char* key) const {
    const auto* feature = findFeature(key);
    return feature && feature->runtimeDisabled;
}

const char* RuntimeFeatureManager::runtimeDisableReason(const char* key) const {
    const auto* feature = findFeature(key);
    return feature ? feature->runtimeReason : nullptr;
}

const RuntimeFeatureManager::FeatureState* RuntimeFeatureManager::findFeature(const char* key) const {
    if (!key) return nullptr;
    for (size_t i = 0; i < count_; ++i) {
        if (strcmp(features_[i].meta.key, key) == 0) return &features_[i];
    }
    return nullptr;
}

// ── 功能注册表（A/B/C/D 类全量注册，compiled 由编译宏决定）────────────────

void RuntimeFeatureManager::generateDefaults() {
    count_ = 0;

    // 出厂默认策略：webServer/mdns 默认启用（管理入口），其余功能 defaultEnabled=false，
    // 用户按实际需求在功能配置页手动启用，实现内存占用最小化与可控性。
    // webServer 在 STA 生产模式下禁用时跳过启动（AP 配置模式豁免，避免失去配置入口）；
    // mDNS 开关与网络模式无关，任何模式下均可启停；两者均支持通过外设执行规则
    // （ACTION_FEATURE_ENABLE/DISABLE，MQTT/GPIO 触发）动态切换并自动重启生效。

    // ═══ A 类: 核心 Web 服务（默认启用，可在功能配置页禁用省内存）═══
    registerFeature({"webServer", "Web 服务 (HTTP/API)", "network", 60, 36864,
                     FASTBEE_ENABLE_WEB_SERVER != 0, true,
                     "AsyncWebServer + routes (~35KB DRAM)", nullptr, 30});
    registerFeature({"mdns", "mDNS 发现 (.local)", "network", 5, 8192,
                     FASTBEE_ENABLE_MDNS != 0, true,
                     "mode-independent switch", nullptr, 8});

    // ═══ B 类：常用外设（出厂默认禁用，按需启用）═══
    registerFeature({"lcd", "OLED/LCD 显示屏", "peripheral", 20, 1200,
                     FASTBEE_ENABLE_LCD != 0, false,
                     "I2C SSD1306/SH1106", nullptr, 19});
    registerFeature({"neopixel", "NeoPixel LED", "peripheral", 12, 212,
                     FASTBEE_ENABLE_NEOPIXEL != 0, false,
                     "GPIO (WS2812)", nullptr, 18});
    registerFeature({"sevenSegment", "TM1637 七段数码管", "peripheral", 3, 32,
                     FASTBEE_ENABLE_SEVEN_SEGMENT != 0, false,
                     "GPIO (2-wire)", nullptr, 0});
    registerFeature({"ds18b20", "DS18B20 温度传感器", "peripheral", 5, 100,
                     (FASTBEE_ENABLE_SENSOR_DRIVER != 0 && FASTBEE_ENABLE_DS18B20 != 0), false,
                     "OneWire 数据引脚 + 外部上拉", nullptr, 0});
    registerFeature({"modbus", "Modbus 主站", "protocol", 20, 1024,
                     FASTBEE_ENABLE_MODBUS != 0, false,
                     "UART/RS485", nullptr, 20, true});
    registerFeature({"ruleScript", "规则引擎（含命令脚本）", "system", 8, 2048,
                     FASTBEE_ENABLE_RULE_SCRIPT != 0, false,
                     nullptr, nullptr, 22, true});

    // ═══ C 类：高级外设与系统功能（出厂默认禁用）═══
    registerFeature({"lcdGb2312", "GB2312 中文字库", "peripheral", 248, 0,
                     FASTBEE_ENABLE_LCD_GB2312 != 0, false,
                     nullptr, "lcd", 0});
    registerFeature({"ledScreen", "RMT LED 大屏", "peripheral", 10, 6144,
                     FASTBEE_ENABLE_LED_SCREEN != 0, false,
                     "ESP32 RMT", nullptr, 15, true});  // 设备大屏默认禁用
    registerFeature({"irRemote", "红外遥控", "peripheral", 8, 300,
                     FASTBEE_ENABLE_IR_REMOTE != 0, false,
                     "IR receiver + GPIO", nullptr, 25, true});
    registerFeature({"rfid", "RFID MFRC522", "peripheral", 12, 200,
                     FASTBEE_ENABLE_RFID != 0, false,
                     "SPI MFRC522", nullptr, 24, true});
    registerFeature({"i2cSensors", "I2C 进阶传感器", "peripheral", 15, 500,
                     FASTBEE_ENABLE_I2C_SENSORS != 0, false,
                     "I2C BMP280/MPU6050", nullptr, 23, true});
    registerFeature({"rfLearn", "433MHz RF 学习", "peripheral", 4, 300,
                     FASTBEE_ENABLE_RF_LEARN != 0, false,
                     "RF receiver + GPIO", nullptr, 12});
    registerFeature({"ds1302", "DS1302 RTC", "peripheral", 2, 100,
                     FASTBEE_ENABLE_DS1302 != 0, false,
                     "3-wire (CE/IO/SCLK)", nullptr, 0});
    registerFeature({"lcd1602", "LCD1602 字符屏", "peripheral", 3, 100,
                     FASTBEE_ENABLE_LCD1602 != 0, false,
                     "I2C PCF8574", nullptr, 0});
    // modbusSlave 从站特性已移除：从站代码由 FASTBEE_MODBUS_SLAVE_ENABLE=0 关闭编译，
    // 该功能项无运行时门控作用（无 shouldEnable("modbusSlave") 调用）且从站数据源未接入，
    // 故不再注册，功能配置界面不显示从站项。
    // commandScript 是规则引擎的附属能力，不再作为独立用户功能展示。
    registerFeature({"commandScript", nullptr, "system", 3, 500,
                     FASTBEE_ENABLE_COMMAND_SCRIPT != 0, false,
                     nullptr, "ruleScript", 20, true});
    // MQTTS 随 MQTT 栈编入（mbedtls 已链接，Flash 增量为 0）；RAM 为 TLS
    // 握手实测 ~42KB（16KB in + 16KB out + ~10KB 上下文），无 PSRAM 时
    // 准入需 ~52KB 空闲（MemoryBudget::MQTTS_NO_PSRAM_MIN_DRAM_FREE）。
    // 出厂默认禁用：scheme=mqtts 的用户需在功能配置页手动启用。
    registerFeature({"mqtts", "MQTTS (加密传输)", "protocol", 0, 43008,
                     FASTBEE_ENABLE_MQTT != 0, false,
                     "TLS handshake ~42KB DRAM (~52KB without PSRAM)", nullptr, 45});
    registerFeature({"deviceControl", "设备控制面板", "system", 27, 4096,
                     FASTBEE_ENABLE_DEVICE_CONTROL != 0, false,
                     "PSRAM required (Web JS ~106KB browser-side)", nullptr, 110});  // 设备大屏控制面板默认禁用
    registerFeature({"ota", "OTA 固件升级", "system", 10, 1024,
                     FASTBEE_ENABLE_OTA != 0, false,
                     "Dual partition + LittleFS support", nullptr, 20});
    // otaFs 作为 ota 的附属功能自动启用
    registerFeature(FeatureMeta{"otaFs", nullptr, "system", 3, 500,
                     (true), false,
                     nullptr, "ota", 0, false});
    registerFeature({"logViewer", "设备日志", "system", 4, 500,
                     FASTBEE_ENABLE_LOG_VIEWER != 0, false,
                     nullptr, nullptr, 0});
    // fileLogging 与日志查看器共用一个用户开关，仍保留独立运行态供内存保护降级。
    registerFeature({"fileLogging", nullptr, "system", 3, 500,
                     FASTBEE_ENABLE_FILE_LOGGING != 0, false,
                     nullptr, "logViewer", 0});
    registerFeature({"userAdmin", "用户管理", "system", 5, 500,
                     FASTBEE_ENABLE_USER_ADMIN != 0, false,
                     nullptr, nullptr, 0});

    // ═══ D 类：网络扩展（出厂默认禁用，以太网/4G 需按硬件配置手动启用）═══
    registerFeature({"ble", "BLE Bluetooth", "network", 80, 10240,
                     FASTBEE_ENABLE_BLE != 0, false,
                     "NimBLE-Arduino", nullptr, 35});
    registerFeature({"ethernet", "以太网 W5500", "network", 8, 2048,
                     FASTBEE_ENABLE_ETHERNET != 0, false,
                     "SPI W5500", nullptr, 15});
    registerFeature({"cellular", "4G Cellular", "network", 15, 4096,
                     FASTBEE_ENABLE_CELLULAR != 0, false,
                     "UART2 EC801E-CN", nullptr, 30});
}

bool RuntimeFeatureManager::mergeLegacyFeatureBundles() {
    bool changed = false;
    for (const auto& bundle : kFeatureBundles) {
        auto* primary = findFeature(bundle.primary);
        auto* member = findFeature(bundle.member);
        if (!primary || !member) continue;

        const bool enabled = primary->userEnabled || member->userEnabled;
        const bool explicitlyConfigured =
            primary->explicitlyConfigured || member->explicitlyConfigured;
        if (primary->userEnabled != enabled ||
            primary->explicitlyConfigured != explicitlyConfigured) {
            changed = true;
        }
        primary->userEnabled = enabled;
        primary->explicitlyConfigured = explicitlyConfigured;
    }
    return changed;
}

bool RuntimeFeatureManager::applyDependencyRules() {
    bool changed = false;
    for (size_t i = 0; i < count_; ++i) {
        if (features_[i].meta.dependsOn) {
            const auto* parent = findFeature(features_[i].meta.dependsOn);
            if (!parent) continue;

            // 无显示名称表示内部附属能力（otaFs、commandScript、fileLogging），
            // 其用户期望状态必须与页面上唯一可见的父功能保持一致。
            if (!features_[i].meta.displayName) {
                if (features_[i].userEnabled != parent->userEnabled ||
                    features_[i].explicitlyConfigured != parent->explicitlyConfigured) {
                    changed = true;
                }
                features_[i].userEnabled = parent->userEnabled;
                features_[i].explicitlyConfigured = parent->explicitlyConfigured;
            } else if (!parent->userEnabled && features_[i].userEnabled) {
                features_[i].userEnabled = false;
                changed = true;
            }
        }
    }
    return changed;
}

void RuntimeFeatureManager::syncBudgetState() {
    auto& budgetReg = FeatureBudgetRegistry::instance();
    for (size_t i = 0; i < count_; ++i) {
        features_[i].budgetPassed = !budgetReg.isDenied(features_[i].meta.key);
    }
}

// ── 配置加载 / 保存 ────────────────────────────────────────────────────

void RuntimeFeatureManager::buildConfigDoc(JsonDocument& doc) const {
    doc["version"] = 1;
    time_t now = time(nullptr);
    if (now > 1577836800) {  // NTP 已同步时才写入时间戳
        char ts[24];
        struct tm tmv;
#if defined(ESP_PLATFORM)
        gmtime_r(&now, &tmv);
#else
        tmv = *gmtime(&now);
#endif
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
        doc["updatedAt"] = ts;
    }
    JsonObject features = doc["features"].to<JsonObject>();
    
    // 保存完整的用户期望状态。配置导出、Web 功能页和 LittleFS 文件因而
    // 使用同一份可读快照；运行期预算/自动降级状态仍只通过状态 API 暴露，
    // 不会覆盖用户选择。
    for (size_t i = 0; i < count_; ++i) {
        JsonObject entry = features[features_[i].meta.key].to<JsonObject>();
        entry["enabled"] = features_[i].userEnabled;
    }
}

bool RuntimeFeatureManager::saveConfig(const JsonDocument& doc) {
    const bool saved = ConfigStorage::getInstance().saveJSONConfig(kConfigPath, doc);
    if (saved) LOGGER.info("[FeatureCfg] Config saved successfully");
    else LOGGER.error("[FeatureCfg] Failed to save verified config and backup");
    return saved;
}

bool RuntimeFeatureManager::saveCurrentConfig() {
    JsonDocument doc;
    buildConfigDoc(doc);
    if (saveConfig(doc)) return true;
    // 写入失败: 重试一次（计划 5.7 文件保护机制）
    LOGGER.warning("[FeatureCfg] Config save failed, retrying once");
    return saveConfig(doc);
}

bool RuntimeFeatureManager::loadConfig() {
    generateDefaults();

    JsonDocument doc;
    if (!ConfigStorage::getInstance().loadJSONConfig(kConfigPath, doc)) {
        const bool hasStoredConfig = LittleFS.exists(kConfigPath) ||
                                     LittleFS.exists(String(kConfigPath) + ".bak");
        if (hasStoredConfig) {
            LOGGER.warning("[FeatureCfg] Primary and backup JSON invalid, using safe defaults");
            loaded_ = true;
            syncBudgetState();
            applyDependencyRules();
            return false;
        }
        // 首次运行: 生成默认配置并写入文件
        LOGGER.info("[FeatureCfg] No config file, generating defaults");
        loaded_ = true;
        syncBudgetState();
        applyDependencyRules();
        saveCurrentConfig();
        return true;
    }

    // 用文件中的值覆盖默认配置。旧版允许只保存少量覆盖项；缺失项继续
    // 使用当前安全默认值，并在本次加载末尾一次性改写为完整快照。
    JsonObjectConst features = doc["features"];
    bool needsCanonicalRewrite = features.isNull() || doc["version"].as<int>() != 1;
    // network.json.enableMDNS was the legacy source of truth.  Do not persist
    // the new default into a missing mdns slot until NetworkManager has had a
    // chance to migrate that legacy value.  Otherwise a reset between the two
    // startup stages turns the generated default into an explicit user value.
    const bool needsLegacyMDNSMigration =
        features.isNull() || !features["mdns"]["enabled"].is<bool>();
    size_t recognizedEntries = 0;
    for (JsonPairConst kv : features) {
        auto* f = findFeature(kv.key().c_str());
        if (f && kv.value()["enabled"].is<bool>()) {
            f->userEnabled = kv.value()["enabled"].as<bool>();
            f->explicitlyConfigured = true;
            ++recognizedEntries;
        } else {
            // 丢弃未知旧键和无效值，避免它们永久污染导出配置。
            needsCanonicalRewrite = true;
        }
    }

    if (recognizedEntries != count_) {
        needsCanonicalRewrite = true;
    }

    loaded_ = true;
    syncBudgetState();
    if (mergeLegacyFeatureBundles()) needsCanonicalRewrite = true;
    if (applyDependencyRules()) needsCanonicalRewrite = true;

    if (needsCanonicalRewrite && !needsLegacyMDNSMigration) {
        LOGGER.info("[FeatureCfg] Normalizing legacy sparse config to full snapshot");
        if (!saveCurrentConfig()) {
            LOGGER.warning("[FeatureCfg] Legacy config loaded but canonical rewrite failed");
        }
    } else if (needsCanonicalRewrite) {
        LOGGER.info("[FeatureCfg] Deferring sparse config rewrite until legacy mDNS migration");
    }
    return true;
}

// ── 状态查询 ───────────────────────────────────────────────────────────

bool RuntimeFeatureManager::shouldEnable(const char* key) const {
    // 安全回退: 若尚未从文件加载（如早期调用），先在内存中注册默认状态，
    // 避免 count_==0 导致所有功能被误禁用
    if (!loaded_) {
        auto* self = const_cast<RuntimeFeatureManager*>(this);
        self->generateDefaults();
        self->syncBudgetState();
        self->applyDependencyRules();
        self->loaded_ = true;
    }
    const auto* f = findFeature(key);
    if (!f) return false;
    if (!(f->meta.compiled && f->userEnabled && f->budgetPassed)) return false;
    if (f->runtimeDisabled) return false;
    // 延迟初始化门控: requiresNetwork 的功能在网络子系统就绪前不激活，
    // 把重型外设/协议的内存分配错峰到 WiFi 启动之后（抬高启动期可用 DRAM）
    if (f->meta.requiresNetwork && !networkReady_) return false;
    // 依赖链检查: 父功能必须同样处于激活状态
    if (f->meta.dependsOn) {
        const auto* parent = findFeature(f->meta.dependsOn);
        if (parent && !(parent->meta.compiled && parent->userEnabled && parent->budgetPassed)) {
            return false;
        }
    }
    return true;
}

bool RuntimeFeatureManager::isUserEnabled(const char* key) const {
    const auto* feature = findFeature(key);
    return feature && feature->userEnabled;
}

bool RuntimeFeatureManager::hasExplicitSetting(const char* key) const {
    const auto* feature = findFeature(key);
    return feature && feature->explicitlyConfigured;
}

bool RuntimeFeatureManager::setUserEnabled(const char* key, bool enabled, bool* changed) {
    if (changed) *changed = false;

    // 兼容旧调用方：无论传入组合主键还是旧成员键，都修改整组功能。
    for (const auto& bundle : kFeatureBundles) {
        if (strcmp(key ? key : "", bundle.primary) != 0 &&
            strcmp(key ? key : "", bundle.member) != 0) {
            continue;
        }
        auto* primary = findFeature(bundle.primary);
        auto* member = findFeature(bundle.member);
        if (!primary || !member) return false;
        if (enabled && (!primary->meta.compiled || !member->meta.compiled)) return false;

        const bool didChange = primary->userEnabled != enabled || member->userEnabled != enabled;
        primary->userEnabled = enabled;
        primary->explicitlyConfigured = true;
        member->userEnabled = enabled;
        member->explicitlyConfigured = true;
        applyDependencyRules();
        if (changed) *changed = didChange;
        return true;
    }

    auto* feature = findFeature(key);
    if (!feature) return false;
    if (enabled && !feature->meta.compiled) return false;

    const bool didChange = feature->userEnabled != enabled;
    feature->userEnabled = enabled;
    feature->explicitlyConfigured = true;
    if (didChange) applyDependencyRules();
    if (changed) *changed = didChange;
    return true;
}

bool RuntimeFeatureManager::resetToDefaults() {
    if (count_ == 0) generateDefaults();

    // Stage the complete default document without changing live state.  A
    // failed flash write must not make the status API claim that reset worked.
    JsonDocument defaults;
    defaults["version"] = 1;
    JsonObject values = defaults["features"].to<JsonObject>();
    for (size_t i = 0; i < count_; ++i) {
        values[features_[i].meta.key]["enabled"] =
            features_[i].meta.compiled && features_[i].meta.defaultEnabled;
    }
    if (!saveConfig(defaults)) {
        LOGGER.warning("[FeatureCfg] Default config save failed, retrying once");
        if (!saveConfig(defaults)) return false;
    }

    for (size_t i = 0; i < count_; ++i) {
        features_[i].userEnabled =
            features_[i].meta.compiled && features_[i].meta.defaultEnabled;
        features_[i].explicitlyConfigured = true;
        features_[i].runtimeDisabled = false;
        features_[i].runtimeReason[0] = '\0';
    }
    loaded_ = true;
    runtimeDegraded_ = false;
    syncBudgetState();
    applyDependencyRules();

    // Factory defaults must not retain a previous generation containing user
    // choices; ConfigStorage normally keeps .bak for ordinary edits.
    const String backupPath = String(kConfigPath) + ".bak";
    const String tmpPath = String(kConfigPath) + ".tmp";
    bool historyRemoved = true;
    if (LittleFS.exists(backupPath) && !LittleFS.remove(backupPath)) historyRemoved = false;
    if (LittleFS.exists(tmpPath) && !LittleFS.remove(tmpPath)) historyRemoved = false;
    if (!historyRemoved) {
        LOGGER.error("[FeatureCfg] Defaults saved but old config history could not be removed");
    }
    return historyRemoved;
}

bool RuntimeFeatureManager::updateFromJson(const JsonDocument& doc) {
    JsonObjectConst features = doc["features"];
    if (features.isNull()) return false;
    bool changed = false;
    for (JsonPairConst kv : features) {
        auto* f = findFeature(kv.key().c_str());
        if (!f) continue;
        // 编译未启用的功能不允许通过 API 开启
        if (!f->meta.compiled) continue;
        if (kv.value()["enabled"].is<bool>()) {
            f->userEnabled = kv.value()["enabled"].as<bool>();
            f->explicitlyConfigured = true;
            changed = true;
        }
    }
    if (changed) {
        // 新页面提交主键；旧调用方可能只提交成员键。主键存在时以主键为准，
        // 否则把成员值提升为整组状态，再同步所有附属能力。
        for (const auto& bundle : kFeatureBundles) {
            JsonObjectConst primaryEntry = features[bundle.primary];
            JsonObjectConst memberEntry = features[bundle.member];
            const bool hasPrimary = !primaryEntry.isNull() &&
                                    primaryEntry["enabled"].is<bool>();
            const bool hasMember = !memberEntry.isNull() &&
                                   memberEntry["enabled"].is<bool>();
            if (!hasPrimary && !hasMember) continue;

            auto* primary = findFeature(bundle.primary);
            if (!primary) continue;
            primary->userEnabled = hasPrimary
                ? primaryEntry["enabled"].as<bool>()
                : memberEntry["enabled"].as<bool>();
            primary->explicitlyConfigured = true;
        }
        applyDependencyRules();
    }
    return changed;
}

RuntimeFeatureManager::UserConfigSnapshot RuntimeFeatureManager::captureUserConfig() const {
    UserConfigSnapshot snapshot;
    snapshot.featureCount = static_cast<uint8_t>(count_);
    for (size_t i = 0; i < count_; ++i) {
        const uint32_t bit = uint32_t{1} << i;
        if (features_[i].userEnabled) snapshot.enabledMask |= bit;
        if (features_[i].explicitlyConfigured) snapshot.explicitMask |= bit;
    }
    return snapshot;
}

bool RuntimeFeatureManager::restoreUserConfig(const UserConfigSnapshot& snapshot) {
    if (snapshot.featureCount != count_) return false;
    for (size_t i = 0; i < count_; ++i) {
        const uint32_t bit = uint32_t{1} << i;
        features_[i].userEnabled = (snapshot.enabledMask & bit) != 0;
        features_[i].explicitlyConfigured = (snapshot.explicitMask & bit) != 0;
    }
    return true;
}

RuntimeFeatureManager::ConfigUpdateResult RuntimeFeatureManager::updateAndSave(
    const JsonDocument& doc, UserConfigSnapshot* previousState) {
    const UserConfigSnapshot before = captureUserConfig();
    if (previousState) *previousState = before;
    if (!updateFromJson(doc)) return ConfigUpdateResult::NO_VALID_ENTRIES;
    if (saveCurrentConfig()) return ConfigUpdateResult::SAVED;

    if (!restoreUserConfig(before)) {
        LOGGER.error("[FeatureCfg] Failed to restore in-memory state after save failure");
    }
    return ConfigUpdateResult::SAVE_FAILED;
}

uint32_t RuntimeFeatureManager::estimateFlashKB() const {
    uint32_t total = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (features_[i].meta.compiled && features_[i].userEnabled) {
            total += features_[i].meta.flashKB;
        }
    }
    return total;
}

uint32_t RuntimeFeatureManager::estimateRamB() const {
    uint32_t total = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (features_[i].meta.compiled && features_[i].userEnabled) {
            total += features_[i].meta.ramB;
        }
    }
    return total;
}

void RuntimeFeatureManager::estimateFromJson(const JsonDocument& doc, uint32_t& flashKB,
                                             uint32_t& ramB) const {
    flashKB = 0;
    ramB = 0;
    JsonObjectConst features = doc["features"];
    for (size_t i = 0; i < count_; ++i) {
        const auto& st = features_[i];
        if (!st.meta.compiled) continue;
        bool enabled = st.userEnabled;
        bool bundleOverride = false;
        if (!features.isNull()) {
            JsonObjectConst entry = features[st.meta.key];
            if (!entry.isNull() && entry["enabled"].is<bool>()) {
                enabled = entry["enabled"].as<bool>();
            }
            // 组合主键与旧成员键必须得到同一个预估值。新请求以主键为准；
            // 兼容请求只传成员键时，成员值提升为整个组合的状态。
            for (const auto& bundle : kFeatureBundles) {
                if (strcmp(st.meta.key, bundle.primary) != 0 &&
                    strcmp(st.meta.key, bundle.member) != 0) {
                    continue;
                }
                JsonObjectConst primaryEntry = features[bundle.primary];
                JsonObjectConst memberEntry = features[bundle.member];
                if (!primaryEntry.isNull() && primaryEntry["enabled"].is<bool>()) {
                    enabled = primaryEntry["enabled"].as<bool>();
                    bundleOverride = true;
                } else if (!memberEntry.isNull() && memberEntry["enabled"].is<bool>()) {
                    enabled = memberEntry["enabled"].as<bool>();
                    bundleOverride = true;
                }
                break;
            }
            // 附属能力由父功能唯一控制。即使旧调用方提交了冲突值，预估也应
            // 与保存后的规范化状态一致，避免页面切换组合功能时少算资源。
            if (!bundleOverride && !st.meta.displayName && st.meta.dependsOn) {
                JsonObjectConst parentEntry = features[st.meta.dependsOn];
                if (!parentEntry.isNull() && parentEntry["enabled"].is<bool>()) {
                    enabled = parentEntry["enabled"].as<bool>();
                } else {
                    const auto* parent = findFeature(st.meta.dependsOn);
                    if (parent) enabled = parent->userEnabled;
                }
            }
        }
        if (enabled) {
            flashKB += st.meta.flashKB;
            ramB += st.meta.ramB;
        }
    }
}

bool RuntimeFeatureManager::getFeatureStatusJson(size_t index, JsonObject obj) const {
    if (index >= count_ || obj.isNull()) return false;

    const auto& st = features_[index];
    obj["key"] = st.meta.key;
    obj["name"] = st.meta.displayName;
    obj["group"] = st.meta.group;
    obj["compiled"] = st.meta.compiled;
    obj["userEnabled"] = st.userEnabled;
    // Report the same effective state that the initialization gates use.
    // A feature can be compiled and user-enabled while still being held
    // back by the startup budget, a dependency, network deferral, or a
    // runtime memory-recovery override. Counting it as active in that
    // situation makes the Web resource summary overstate live usage.
    const bool active = shouldEnable(st.meta.key);
    obj["active"] = active;
    obj["runtimeDisabled"] = st.runtimeDisabled;
    if (st.runtimeDisabled && st.runtimeReason[0] != '\0') {
        obj["runtimeDisableReason"] = st.runtimeReason;
    }
    uint32_t displayFlashKB = st.meta.flashKB;
    uint32_t displayRamB = st.meta.ramB;
    if (!st.meta.displayName && st.meta.dependsOn) {
        // 附属项仍保留在状态 API 中供诊断，但资源归入可见父项，避免前端
        // 在组合功能行中重复统计。
        displayFlashKB = 0;
        displayRamB = 0;
    } else if (st.meta.displayName) {
        for (size_t i = 0; i < count_; ++i) {
            const auto& child = features_[i];
            if (!child.meta.displayName && child.meta.dependsOn &&
                strcmp(child.meta.dependsOn, st.meta.key) == 0 && child.meta.compiled) {
                displayFlashKB += child.meta.flashKB;
                displayRamB += child.meta.ramB;
            }
        }
    }
    obj["flashKB"] = displayFlashKB;
    obj["ramB"] = displayRamB;
    if (st.meta.hwDep) obj["hwDep"] = st.meta.hwDep;
    if (st.meta.dependsOn) obj["dependsOn"] = st.meta.dependsOn;
    // 延迟初始化标记：true 表示该功能在网络就绪后才激活（错峰启动）
    if (st.meta.requiresNetwork) obj["deferred"] = true;
    if (!st.meta.compiled) {
        obj["locked"] = true;
        // OTA 在 4MB 单 app 分区中是硬件/分区能力限制，不是统一代码
        // 缺失；将原因透传给页面，避免误导用户重新编译无效固件。
        if (st.meta.key && strcmp(st.meta.key, "ota") == 0) {
            obj["lockReason"] = "当前分区不支持 OTA（需要带 ota_1 的 A/B 双分区）";
        } else {
            obj["lockReason"] = "Not compiled into firmware";
        }
    }
    return true;
}

void RuntimeFeatureManager::getStatusSummaryJson(JsonObject summary) const {
    if (summary.isNull()) return;

    uint32_t totalFlashKB = 0;
    uint32_t activeFlashKB = 0;
    uint32_t activeRamB = 0;
    for (size_t i = 0; i < count_; ++i) {
        const auto& st = features_[i];
        if (st.meta.compiled) totalFlashKB += st.meta.flashKB;
        const bool active = shouldEnable(st.meta.key);
        if (active) {
            activeFlashKB += st.meta.flashKB;
            activeRamB += st.meta.ramB;
        }
    }

    summary["totalFlashKB"] = totalFlashKB;
    summary["activeFlashKB"] = activeFlashKB;
    summary["activeRamB"] = activeRamB;
    summary["runtimeDegraded"] = runtimeDegraded_;
#if defined(ESP_PLATFORM)
    summary["free8KB"] = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
    summary["largest8KB"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024;
#endif
}

void RuntimeFeatureManager::getStatusJson(JsonVariant dst) const {
    dst.to<JsonObject>();
    JsonArray arr = dst["features"].to<JsonArray>();
    for (size_t i = 0; i < count_; ++i) {
        getFeatureStatusJson(i, arr.add<JsonObject>());
    }
    getStatusSummaryJson(dst["summary"].to<JsonObject>());
}

}  // namespace FastBee
