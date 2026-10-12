#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FastBee {
// Snapshot currently buffered bytes without waiting for a frame/terminator.
// Failure may consume transport bytes but never publishes a partial sample.
template<size_t Limit, class Stream, class Validator>
bool readBufferedStreamChecked(Stream& stream, uint8_t* destination, size_t capacity, size_t& length,
                               Validator current) {
    static_assert(Limit > 0, "A finite staging buffer is required");
    length = 0;
    if (!destination || !capacity || !current()) return false;
    const int available = stream.available();
    if (available < 0) return false;
    size_t requested = static_cast<size_t>(available);
    if (requested > capacity) requested = capacity;
    if (requested > Limit) requested = Limit;
    uint8_t staged[Limit];
    for (size_t i = 0; i < requested; ++i) {
        const int value = stream.read();
        if (value < 0 || value > 255) return false;
        staged[i] = static_cast<uint8_t>(value);
    }
    if (!current()) return false;
    if (requested) std::memcpy(destination, staged, requested);
    length = requested;
    return true; // Empty UART buffer is a successful no-data poll, not a sample.
}

template<size_t Limit, class Stream>
bool readBufferedStream(Stream& stream, uint8_t* destination, size_t capacity, size_t& length) {
    return readBufferedStreamChecked<Limit>(stream, destination, capacity, length, [] { return true; });
}

template<class Stream> void discardBufferedStream(Stream& stream) {
    for (unsigned i = 0; i < 255 && stream.available() > 0; ++i)
        if (stream.read() < 0) break;
}

template<size_t Limit, class Stream>
bool readExactBufferedStream(Stream& stream, uint8_t* destination, size_t requested) {
    if (!destination || !requested || requested > Limit || stream.available() != static_cast<int>(requested)) {
        discardBufferedStream(stream); return false;
    }
    uint8_t staged[Limit];
    size_t read = 0;
    if (!readBufferedStream<Limit>(stream, staged, requested, read) || read != requested || stream.available() != 0) {
        discardBufferedStream(stream); return false;
    }
    std::memcpy(destination, staged, requested);
    return true;
}
} // namespace FastBee
