#pragma once
#include <atomic>
#include <cstdint>

namespace FastBee {
// Published only by successful saved-layout initialization/commit. Readers
// never take PanelLock while PeripheralManager owns its I/O lock.
class ScreenOutputIdentity {
public:
    static uint32_t current() { return currentRevision().load(std::memory_order_acquire); }
    static void publish(uint32_t revision) {
        currentRevision().store(revision == UINT32_MAX ? 0 : revision, std::memory_order_release);
    }
    static bool matches(uint64_t actor) {
        const uint32_t revision = static_cast<uint32_t>(actor >> 32U);
        return revision && static_cast<uint32_t>(actor) &&
            revision == currentRevision().load(std::memory_order_acquire);
    }
private:
    static std::atomic<uint32_t>& currentRevision() {
        static std::atomic<uint32_t> revision{0};
        return revision;
    }
};
} // namespace FastBee
