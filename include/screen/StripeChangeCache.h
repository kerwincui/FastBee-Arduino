#pragma once

#include <cstddef>
#include <cstdint>

namespace FastBee { namespace Screen {

// Metadata only: no second framebuffer and no ownership of the supplied storage.
// An unavailable cache simply preserves the original flush behavior.
class StripeChangeCache {
public:
    struct Entry {
        uint32_t hash;
        bool valid;
    };

    void reset(Entry* entries = nullptr, size_t count = 0) {
        _entries = entries;
        _count = entries ? count : 0;
        invalidate();
    }

    void invalidate() {
        for (size_t i = 0; i < _count; ++i) _entries[i].valid = false;
    }

    template<class Flush>
    bool submit(size_t index, const uint16_t* pixels, size_t count, Flush flush) {
        if (!_entries || index >= _count) return flush();
        uint32_t hash = 2166136261UL;
        for (size_t i = 0; i < count; ++i) {
            hash = (hash ^ (pixels[i] & 0xffU)) * 16777619UL;
            hash = (hash ^ (pixels[i] >> 8)) * 16777619UL;
        }
        Entry& entry = _entries[index];
        if (entry.valid && entry.hash == hash) return true;
        // A failed backend submission may already have changed some pixels.
        entry.valid = false;
        if (!flush()) return false;
        entry.hash = hash;
        entry.valid = true;
        return true;
    }

private:
    Entry* _entries = nullptr;
    size_t _count = 0;
};

} }
