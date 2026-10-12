#pragma once
namespace FastBee {
struct RuntimeFeatureManager {
    bool allowed=true;
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager m; return m; }
    bool shouldEnable(const char*) const { return allowed; }
};
}
