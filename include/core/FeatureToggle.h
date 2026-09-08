/**
 * @file FeatureToggle.h
 * @brief Feature Toggle Engine for runtime feature management
 * @note Compatibility facade over RuntimeFeatureManager. Keeping one source
 * of truth avoids two independent feature registries drifting apart.
 */

#pragma once

#include <cstdint>
#include "core/RuntimeFeatureManager.h"

namespace FastBee {

class FeatureToggleEngine {
public:
    static FeatureToggleEngine& instance() {
        static FeatureToggleEngine inst;
        return inst;
    }
    
    bool initialize() {
        initialized_ = true;
        return true;
    }
    
    bool isEnabled(const char* feature) const {
        if (!initialized_ || !feature) return false;
        return RuntimeFeatureManager::instance().shouldEnable(feature);
    }
    
private:
    FeatureToggleEngine() = default;
    ~FeatureToggleEngine() = default;
    FeatureToggleEngine(const FeatureToggleEngine&) = delete;
    FeatureToggleEngine& operator=(const FeatureToggleEngine&) = delete;
    bool initialized_ = false;
};

} // namespace FastBee
