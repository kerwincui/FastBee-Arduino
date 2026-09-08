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
}  // namespace

namespace FastBee {

// ── 注册与查找 ─────────────────────────────────────────────────────────

void RuntimeFeatureManager::registerFeature(const FeatureMeta& meta) {
    if (count_ >= kMaxFeatures) return;
    FeatureState& st = features_[count_++];
    st.meta = meta;
    st.userEnabled = meta.compiled && meta.defaultEnabled;
    st.budgetPassed = true;
}

RuntimeFeatureManager::FeatureState* RuntimeFeatureManager::findFeature(const char* key) {
    if (!key) return nullptr;
    for (size_t i = 0; i < count_; ++i) {
        if (strcmp(features_[i].meta.key, key) == 0) return &features_[i];
    }
    return nullptr;
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
    registerFeature({"ruleScript", "规则引擎", "system", 8, 2048,
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
    registerFeature({"commandScript", "命令脚本", "system", 3, 500,
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
    registerFeature({"logViewer", "日志查看器", "system", 4, 500,
                     FASTBEE_ENABLE_LOG_VIEWER != 0, false,
                     nullptr, nullptr, 0});
    registerFeature({"fileLogging", "文件日志记录", "system", 3, 500,
                     FASTBEE_ENABLE_FILE_LOGGING != 0, false,
                     nullptr, nullptr, 0});
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

void RuntimeFeatureManager::applyDependencyRules() {
    // 依赖检查: 父功能未启用则子功能自动禁用
    for (size_t i = 0; i < count_; ++i) {
        if (features_[i].meta.dependsOn) {
            const auto* parent = findFeature(features_[i].meta.dependsOn);
            if (parent && !parent->userEnabled) {
                features_[i].userEnabled = false;
            }
        }
    }
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
    
    // WP-93: 稀疏覆盖 - 只保存用户显式修改的项，不写入默认值
    for (size_t i = 0; i < count_; ++i) {
        // 只有当 userEnabled 与 defaultEnabled 不同时才写入
        if (features_[i].userEnabled != features_[i].meta.defaultEnabled) {
            JsonObject entry = features[features_[i].meta.key].to<JsonObject>();
            entry["enabled"] = features_[i].userEnabled;
        }
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

    // 用文件中的值覆盖默认配置
    JsonObjectConst features = doc["features"];
    for (JsonPairConst kv : features) {
        auto* f = findFeature(kv.key().c_str());
        if (f && kv.value()["enabled"].is<bool>()) {
            f->userEnabled = kv.value()["enabled"].as<bool>();
        }
    }

    // 升级兼容迁移：旧版固件的 features.json 不含 webServer/mdns 键（当时无此开关，
    // Web 服务常驻）。若直接套用新默认值（禁用），升级后的 STA 设备将失去 Web 入口
    // 且无法通过页面重新启用。因此旧配置文件缺失这两个键时保持启用，
    // 并回写文件完成迁移；全新部署（无文件）仍遵循出厂全禁用策略。
    if (features["webServer"].isNull() || features["mdns"].isNull()) {
        const char* legacyKeys[] = {"webServer", "mdns"};
        for (const char* key : legacyKeys) {
            if (features[key].isNull()) {
                auto* f = findFeature(key);
                if (f) f->userEnabled = true;
            }
        }
        loaded_ = true;
        syncBudgetState();
        applyDependencyRules();
        LOGGER.info("[FeatureCfg] Migrated legacy config: webServer/mdns kept enabled");
        saveCurrentConfig();
        return true;
    }

    loaded_ = true;
    syncBudgetState();
    applyDependencyRules();
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
            changed = true;
        }
    }
    if (changed) applyDependencyRules();
    return changed;
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
        if (!features.isNull()) {
            JsonObjectConst entry = features[st.meta.key];
            if (!entry.isNull() && entry["enabled"].is<bool>()) {
                enabled = entry["enabled"].as<bool>();
            }
        }
        if (enabled) {
            flashKB += st.meta.flashKB;
            ramB += st.meta.ramB;
        }
    }
}

void RuntimeFeatureManager::getStatusJson(JsonVariant dst) const {
    dst.to<JsonObject>();
    JsonArray arr = dst["features"].to<JsonArray>();
    uint32_t totalFlashKB = 0;
    uint32_t activeFlashKB = 0;
    uint32_t activeRamB = 0;

    for (size_t i = 0; i < count_; ++i) {
        const auto& st = features_[i];
        if (st.meta.compiled) totalFlashKB += st.meta.flashKB;
        JsonObject obj = arr.add<JsonObject>();
        obj["key"] = st.meta.key;
        obj["name"] = st.meta.displayName;
        obj["group"] = st.meta.group;
        obj["compiled"] = st.meta.compiled;
        obj["userEnabled"] = st.userEnabled;
        obj["active"] = shouldEnable(st.meta.key);
        obj["flashKB"] = st.meta.flashKB;
        obj["ramB"] = st.meta.ramB;
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
        if (st.meta.compiled && st.userEnabled) {
            activeFlashKB += st.meta.flashKB;
            activeRamB += st.meta.ramB;
        }
    }

    JsonObject summary = dst["summary"].to<JsonObject>();
    summary["totalFlashKB"] = totalFlashKB;
    summary["activeFlashKB"] = activeFlashKB;
    summary["activeRamB"] = activeRamB;
#if defined(ESP_PLATFORM)
    summary["free8KB"] = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
    summary["largest8KB"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024;
#endif
}

}  // namespace FastBee
