#pragma once
namespace FastBee {
class FeatureBudgetRegistry {
public:
    static FeatureBudgetRegistry& instance() { static FeatureBudgetRegistry value; return value; }
    bool isDenied(const char*) const { return false; }
};
}
