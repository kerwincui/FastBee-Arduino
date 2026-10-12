#pragma once
#include <cstddef>
#include <cstdint>

namespace FastBee {

// Text data-source framing, not a binary UART protocol decoder. A damaged or
// oversized line must never become several independently actionable values.
template <size_t Capacity>
class UartTextFramer {
public:
    static_assert(Capacity > 0, "UART frame capacity must be nonzero");
    enum class Result { None, Ready, TooLong, InvalidByte };
    static constexpr uint32_t IdleMs = 500;

    Result push(uint8_t byte, uint32_t now) {
        lastByteAt_ = now;
        active_ = true;
        if (byte == '\r' || byte == '\n') {
            active_ = false;
            if (discarding_) { clear(); return Result::None; }
            if (length_) return Result::Ready;
            return Result::None;
        }
        if (discarding_) return Result::None;
        // Embedded NUL cannot be passed through String/JSON as a text value.
        if (!byte) { discard(now); return Result::InvalidByte; }
        if (length_ == Capacity) { discard(now); return Result::TooLong; }
        bytes_[length_++] = static_cast<char>(byte);
        bytes_[length_] = '\0';
        return Result::None;
    }

    // Call only after a successful empty driver read; a capped nonempty read
    // cannot prove an idle boundary while more bytes may be queued in hardware.
    Result idle(uint32_t now) {
        if (!active_ || uint32_t(now - lastByteAt_) < IdleMs) return Result::None;
        active_ = false;
        if (discarding_) { clear(); return Result::None; }
        return length_ ? Result::Ready : Result::None;
    }
    // Read failures or source changes invalidate the partial line. Drain to a
    // delimiter/observed idle boundary before admitting another text frame.
    void discard(uint32_t now) {
        length_ = 0; bytes_[0] = '\0'; discarding_ = true;
        active_ = true; lastByteAt_ = now;
    }
    void clear() {
        length_ = 0; bytes_[0] = '\0'; active_ = false; discarding_ = false;
    }
    const char* data() const { return bytes_; }
    size_t size() const { return length_; }
    bool discarding() const { return discarding_; }
private:
    char bytes_[Capacity + 1] = {};
    size_t length_ = 0;
    uint32_t lastByteAt_ = 0;
    bool active_ = false, discarding_ = false;
};
} // namespace FastBee
