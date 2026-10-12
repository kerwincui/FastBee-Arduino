#pragma once

#include "core/BoardProfile.h"
#include <stddef.h>
#include <stdint.h>

namespace FastBee {

// Immutable profile data is streamed through one fixed scratch buffer. No JSON
// tree, catalog-sized String or per-pin heap allocation survives a request.
class BoardProfileCatalog {
public:
    BoardProfileCatalog(const BoardProfile* active, const char* compiledChip);
    size_t measureLength();
    size_t read(uint8_t* destination, size_t capacity);
    bool valid() const { return valid_; }
    void rewind();

private:
    enum class Stage : uint8_t { ENVELOPE, PROFILE, PIN, PROFILE_END, END, DONE };
    bool nextFragment();
    const BoardProfile* profiles_[7];
    const BoardProfile* active_;
    const char* compiledChip_;
    char fragment_[1536];
    size_t profile_ = 0;
    size_t pin_ = 0;
    size_t length_ = 0;
    size_t offset_ = 0;
    Stage stage_ = Stage::ENVELOPE;
    bool valid_ = true;
};

} // namespace FastBee
