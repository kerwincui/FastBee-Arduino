/**
 * @file RuntimeFeatureManager.h
 * @brief 运行时功能配置管理器（双层控制架构的运行层）
 *
 * 功能启用采用 编译时宏 + 运行时配置 双层控制：
 *   决策链: compiled(编译宏) AND userEnabled(features.json) AND budgetPassed(FeatureBudget)
 *
 * - 用户配置持久化于 LittleFS /config/features.json
 * - 文件不存在 -> generateDefaults() 生成默认配置并写入
 * - JSON 损坏   -> 回退默认配置并记录警告
 * - 各驱动初始化入口通过 shouldEnable(key) 门控，未启用则跳过初始化（节省 RAM）
 */

#ifndef FASTBEE_RUNTIME_FEATURE_MANAGER_H
#define FASTBEE_RUNTIME_FEATURE_MANAGER_H

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

namespace FastBee {

class RuntimeFeatureManager {
public:
    static RuntimeFeatureManager& instance() {
        static RuntimeFeatureManager mgr;
        return mgr;
    }

    // 启动时从 /config/features.json 加载用户配置
    // 文件不存在 -> 调用 generateDefaults() 生成默认配置并写入
    // 文件损坏 (JSON 解析失败) -> 回退到默认配置并记录警告
    bool loadConfig();

    // 三层判断: compiled AND userEnabled AND budgetPassed
    // 任一为 false 则功能不初始化
    bool shouldEnable(const char* featureName) const;

    // 获取所有功能的完整状态 (供 /api/features/status)
    // 返回每个功能: { key, name, group, compiled, userEnabled, active,
    //                 flashKB, ramB, hwDep, dependsOn }
    void getStatusJson(JsonVariant dst) const;

    // 用请求 JSON 中的 features 字段更新内存中的 userEnabled 状态
    // 请求格式: { "features": { "<key>": { "enabled": bool } } }
    bool updateFromJson(const JsonDocument& doc);

    // 将指定文档写入 /config/features.json（写临时文件+验证+原子替换，见计划 12.4）
    bool saveConfig(const JsonDocument& doc);

    // 将当前内存状态序列化并持久化（首次运行/配置变更后调用）
    bool saveCurrentConfig();

    // 估算当前启用功能的总 Flash/RAM 占用 (供 Web UI 实时显示)
    uint32_t estimateFlashKB() const;
    uint32_t estimateRamB() const;

    // 按给定请求 JSON 估算启用后的 Flash/RAM 占用（不修改当前状态）
    void estimateFromJson(const JsonDocument& doc, uint32_t& flashKB, uint32_t& ramB) const;

    // 功能元数据
    struct FeatureMeta {
        const char* key;           // camelCase API 键名 (与 features.json 一致)
        const char* displayName;   // 显示名称
        const char* group;         // 分组: "peripheral" / "protocol" / "network" / "system"
        uint16_t flashKB;          // 预估 Flash 占用
        uint32_t ramB;             // 预估 RAM 占用
        bool compiled;             // 编译时是否编入 (FASTBEE_ENABLE_xxx)
        bool defaultEnabled;       // 默认启用状态 (B 类=true, C/D 类=false)
        const char* hwDep;         // 硬件依赖描述 (可 nullptr)
        const char* dependsOn;     // 依赖的其他功能 key (可 nullptr)
        uint16_t minDramKB;        // 最低 DRAM 要求 (KB, 0=无要求)
    };

    static constexpr size_t kMaxFeatures = 28;  // 覆盖所有 B/C/D 类功能

    bool loaded() const { return loaded_; }
    size_t featureCount() const { return count_; }

private:
    struct FeatureState {
        FeatureMeta meta;
        bool userEnabled;          // 用户配置 (features.json)
        bool budgetPassed;         // FeatureBudget 校验结果
    };

    FeatureState features_[kMaxFeatures];
    size_t count_ = 0;
    bool loaded_ = false;

    RuntimeFeatureManager() = default;
    void registerFeature(const FeatureMeta& meta);
    void generateDefaults();       // 注册所有功能并设置默认启用状态
    void syncBudgetState();        // 从 FeatureBudgetRegistry 同步 budgetPassed
    FeatureState* findFeature(const char* key);
    const FeatureState* findFeature(const char* key) const;
    void buildConfigDoc(JsonDocument& doc) const;  // 序列化当前状态
    void applyDependencyRules();   // 依赖检查: 父功能禁用则子功能自动禁用
};

}  // namespace FastBee

#endif  // FASTBEE_RUNTIME_FEATURE_MANAGER_H
