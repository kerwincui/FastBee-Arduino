#pragma once
namespace FastBee {
struct RuntimeFeatureManager {
    static inline bool enabled = true;
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager x; return x; }
    bool shouldEnable(const char*) const { return enabled; }
};
}
