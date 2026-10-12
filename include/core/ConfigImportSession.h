#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FastBee {

// One bounded ordinary upload per process. All access (including filesystem
// operations) must hold Guard. Tokens are issued by the server at begin, never
// admitted implicitly by a data chunk. They are not authentication credentials.
class ConfigImportSession {
public:
    static constexpr uint32_t MaxBytes = 24 * 1024;
    static constexpr uint32_t MaxChunks = 128;
    static constexpr uint32_t TimeoutMs = 60000;
    enum class Phase { Empty, Receiving, Committed, Failed };
    enum class Admission { Reject, Append, ReplayLast };

    class Guard {
    public:
        explicit Guard(ConfigImportSession& owner) : owner_(owner),
            acquired_(!owner_.lock_.test_and_set(std::memory_order_acquire)) {}
        ~Guard() { if (acquired_) owner_.lock_.clear(std::memory_order_release); }
        explicit operator bool() const { return acquired_; }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        ConfigImportSession& owner_;
        bool acquired_;
    };

    static bool decimal(const char* text, uint32_t limit, uint32_t& value) {
        if (!text || !*text || (text[0] == '0' && text[1])) return false;
        uint32_t result = 0;
        for (size_t i = 0; text[i]; ++i) {
            if (i >= 6 || text[i] < '0' || text[i] > '9') return false;
            result = result * 10 + static_cast<uint32_t>(text[i] - '0');
            if (result > limit) return false;
        }
        value = result;
        return true;
    }
    static bool validToken(const char* value) {
        if (!value || std::strlen(value) != 32) return false;
        for (size_t i = 0; i < 32; ++i)
            if (!((value[i] >= '0' && value[i] <= '9') ||
                  (value[i] >= 'a' && value[i] <= 'f'))) return false;
        return true;
    }
    bool busy(uint32_t now) const {
        return phase == Phase::Receiving && !expired(now);
    }
    bool expired(uint32_t now) const { return uint32_t(now - touched) >= TimeoutMs; }
    bool begin(const char* token, const char* target, uint32_t count,
               uint32_t bytes, uint32_t now) {
        if (busy(now) || !validToken(token) || !target || !*target ||
            std::strlen(target) > 48 || !count || count > MaxChunks ||
            !bytes || bytes > MaxBytes || count > bytes) return false;
        std::strcpy(session, token);
        std::strcpy(name, target);
        total = count; expectedSize = bytes;
        nextIndex = received = lastOffset = lastLength = 0;
        phase = Phase::Receiving; touched = now;
        return true;
    }
    Admission inspect(const char* token, const char* target, uint32_t index,
                      uint32_t count, uint32_t length, uint32_t now) const {
        if (!validToken(token) || !target || std::strcmp(token, session) ||
            std::strcmp(target, name) || count != total || index >= total ||
            !length || expired(now) || phase == Phase::Empty || phase == Phase::Failed)
            return Admission::Reject;
        if (nextIndex && index == nextIndex - 1 && length == lastLength)
            return Admission::ReplayLast; // Caller must compare stored bytes.
        if (phase != Phase::Receiving || index != nextIndex ||
            length > expectedSize - received) return Admission::Reject;
        const uint32_t end = received + length;
        if ((index == total - 1) != (end == expectedSize)) return Admission::Reject;
        // Every remaining chunk must have at least one byte.
        if (expectedSize - end < total - index - 1) return Admission::Reject;
        return Admission::Append;
    }
    void appended(uint32_t length, uint32_t now) {
        lastOffset = received; lastLength = length;
        received += length; ++nextIndex; touched = now;
    }
    void failed() { phase = Phase::Failed; }
    void committed() { phase = Phase::Committed; }
    void invalidate() { phase = Phase::Empty; session[0] = '\0'; }

    char session[33] = {};
    char name[49] = {};
    uint32_t total = 0, expectedSize = 0, nextIndex = 0, received = 0;
    uint32_t lastOffset = 0, lastLength = 0, touched = 0;
    Phase phase = Phase::Empty;
private:
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};
} // namespace FastBee
