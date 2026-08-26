/**
 * @file FeatureBudget.h
 * @brief 运行时功能内存预算校验
 *
 * 用户可通过 env 覆盖启用重型功能（如 F4R0 上 FASTBEE_ENABLE_IR_REMOTE=1），
 * 但编译期无法感知运行时 DRAM 水位。启动时按 DRAM 需求从大到小逐个校验
 * 编译启用的功能：内存不足则记入拒绝表并输出 [BUDGET] 警告，对应驱动
 * 初始化入口（IRRemoteDriver/RFIDDriver/LCDManager/NeoPixel/Modbus 等）
 * 通过 isDenied() 自动降级跳过，不影响其他功能稳定性。
 */

#ifndef FASTBEE_FEATURE_BUDGET_H
#define FASTBEE_FEATURE_BUDGET_H

#include <stddef.h>
#include <string.h>

namespace FastBee {

// 单个功能的内存预算：启用该功能所需的最小 free8（KB，MALLOC_CAP_8BIT 口径）
struct FeatureBudget {
    const char* name;        // API 键名（camelCase，与 /api/system/status 一致）
    uint16_t minFree8Kb;     // 最低 free8 要求（KB）
};

// 功能运行时状态（供 /api/system/status features 字段使用）
// 注意：成员名避免与 esp32-hal-gpio.h 的 DISABLED 宏冲突
enum class FeatureRuntimeState : uint8_t {
    Disabled = 0,    // 编译禁用（代码未编入固件）
    Active = 1,      // 编译启用且预算通过
    NoMemory = 2     // 编译启用但内存不足，自动降级
};

class FeatureBudgetRegistry {
public:
    static FeatureBudgetRegistry& instance() {
        static FeatureBudgetRegistry registry;
        return registry;
    }

    // 启动时内存预算校验（FastBeeFramework::initialize 中 WiFi/MQTT 之前调用）。
    // 检查顺序按 DRAM 需求从大到小，确保重型功能优先获得内存。
    // 定义见 FastBeeFramework.cpp。
    void runChecks();

    // 周期性重评估：对已通过的功能重新检查，DRAM 下降则加入 denied 表
    // 已 denied 的不重新放行（需重启恢复，避免振荡）
    void recheck();

    bool checked() const { return checked_; }

    // 功能是否被预算拒绝（编译启用但内存不足）
    bool isDenied(const char* name) const {
        if (!name) return false;
        for (size_t i = 0; i < deniedCount_; ++i) {
            if (strcmp(denied_[i], name) == 0) return true;
        }
        return false;
    }

private:
    FeatureBudgetRegistry() = default;

    static constexpr size_t kMaxDenied = 16;
    const char* denied_[kMaxDenied] = {nullptr};
    size_t deniedCount_ = 0;
    bool checked_ = false;

    friend class FeatureBudgetRegistryAccess;
};

// 供 FastBeeFramework.cpp 内部填充拒绝表
class FeatureBudgetRegistryAccess {
public:
    static void markChecked() { FeatureBudgetRegistry::instance().checked_ = true; }
    static void deny(const char* name) {
        if (!name) return;
        auto& r = FeatureBudgetRegistry::instance();
        if (r.deniedCount_ < FeatureBudgetRegistry::kMaxDenied) {
            r.denied_[r.deniedCount_++] = name;
        }
    }
};

}  // namespace FastBee

#endif  // FASTBEE_FEATURE_BUDGET_H
