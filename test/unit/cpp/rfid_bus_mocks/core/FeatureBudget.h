#pragma once
namespace FastBee {
struct FeatureBudgetRegistry {
    static inline bool denied = false;
    static FeatureBudgetRegistry& instance() { static FeatureBudgetRegistry x; return x; }
    bool isDenied(const char*) const { return denied; }
};
}
