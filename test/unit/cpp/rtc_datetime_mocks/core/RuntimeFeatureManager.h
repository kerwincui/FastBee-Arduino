#pragma once
namespace FastBee {
struct RuntimeFeatureManager {
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager m; return m; }
    bool shouldEnable(const char*) const { return true; }
};
}
