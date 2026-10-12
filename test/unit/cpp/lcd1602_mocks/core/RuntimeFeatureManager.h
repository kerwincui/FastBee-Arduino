#pragma once
namespace FastBee {
struct RuntimeFeatureManager {
    bool enabled = true;
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager manager; return manager; }
    bool shouldEnable(const char*) const { return enabled; }
};
}
