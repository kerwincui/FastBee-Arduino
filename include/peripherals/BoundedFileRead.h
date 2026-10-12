#pragma once
#include <Arduino.h>
#include <utility>

namespace FastBee {
enum class FileReadResult { Ok, TooLarge, NoMemory, Incomplete, Changed };

// Publish a complete file snapshot only. Caller holds the filesystem/bus lock
// for the whole operation and closes the file on every result. A failed read
// leaves the previous destination untouched; it must not be returned as new data.
template<class File>
FileReadResult readWholeFile(File& file, size_t limit, String& destination) {
    const size_t expected = file.size();
    if (expected > limit) return FileReadResult::TooLarge;
    String staged;
    if (expected && !staged.reserve(expected)) return FileReadResult::NoMemory;
    for (size_t i = 0; i < expected; ++i) {
        if (file.available() <= 0) return FileReadResult::Incomplete;
        const int value = file.read();
        if (value < 0 || value > 255) return FileReadResult::Incomplete;
        if (!staged.concat(static_cast<char>(value)) || staged.length() != i + 1)
            return FileReadResult::NoMemory;
    }
    // Reject growth as well as shrinkage. Never trust an endless available()
    // result, or publish a prefix as a successful whole-file read.
    if (file.size() != expected || file.available() != 0) return FileReadResult::Changed;
    destination = std::move(staged);
    return FileReadResult::Ok;
}
} // namespace FastBee
