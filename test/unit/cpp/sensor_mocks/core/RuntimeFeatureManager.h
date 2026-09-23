#pragma once
namespace FastBee {
class RuntimeFeatureManager {
public:
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager value; return value; }
    bool shouldEnable(const char*) const { return enabled; }
    bool enabled = true;
};
}
