#pragma once

#include "core/RuntimeFeatureManager.h"
#include <memory>

namespace FastBee {

// Capture passive status once, then stream immutable records without retaining
// every serialized String. All allocation/fragment checks precede HTTP 200.
class FeatureStatusCatalog {
public:
    explicit FeatureStatusCatalog(const RuntimeFeatureManager& manager);
    size_t measureLength();
    size_t read(uint8_t* destination, size_t capacity);
    bool valid() const { return valid_; }

private:
    struct Snapshot {
        const char* key = nullptr;
        const char* name = nullptr;
        const char* group = nullptr;
        const char* hwDep = nullptr;
        const char* dependsOn = nullptr;
        const char* admission = nullptr;
        const char* instanceState = nullptr;
        const char* instanceScope = nullptr;
        const char* instanceReason = nullptr;
        uint32_t flashKB = 0, ramB = 0;
        uint16_t flags = 0, initializedCount = 0;
        std::unique_ptr<char[]> runtimeReason;
    };
    bool nextFragment();
    void rewind();
    std::unique_ptr<Snapshot[]> snapshots_;
    size_t count_ = 0, record_ = 0, length_ = 0, offset_ = 0;
    size_t measuredLength_ = 0;
    uint32_t totalFlashKB_ = 0, activeFlashKB_ = 0, activeRamB_ = 0;
    uint32_t free8KB_ = 0, largest8KB_ = 0;
    bool runtimeDegraded_ = false, hasHeap_ = false, valid_ = false;
    bool measured_ = false;
    uint8_t phase_ = 0;
    char fragment_[1536];
};

} // namespace FastBee
