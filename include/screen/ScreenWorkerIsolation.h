#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace FastBee { namespace Screen {

// Temporary diagnostic policy. This header is not included by the normal
// screen runtime unless FASTBEE_SCREEN_WORKER_ISOLATION is explicitly 1.
class ScreenWorkerIsolation {
public:
    enum class Phase : uint8_t { Hold, Snapshot, Render, Normal };
    static constexpr uint32_t PHASE_MS = 90000;
    static constexpr uint32_t TOTAL_MS = PHASE_MS * 3;

    explicit ScreenWorkerIsolation(uint32_t startedMs) : _startedMs(startedMs) {}

    Phase phase(uint32_t nowMs) {
        if (_complete) return Phase::Normal;
        const uint32_t elapsed = nowMs - _startedMs;
        if (elapsed >= TOTAL_MS) {
            _complete = true;
            return Phase::Normal;
        }
        if (elapsed >= PHASE_MS * 2) return Phase::Render;
        if (elapsed >= PHASE_MS) return Phase::Snapshot;
        return Phase::Hold;
    }

    bool needsPhaseFrame(Phase phase) const {
        return _lastDrawnPhase != static_cast<uint8_t>(phase);
    }
    bool allowSnapshot(Phase phase) const {
        return phase != Phase::Hold || needsPhaseFrame(phase);
    }
    bool allowRender(Phase phase) const {
        return phase == Phase::Render || phase == Phase::Normal || needsPhaseFrame(phase);
    }
    static bool allowTouch(Phase phase) { return phase == Phase::Normal; }
    void rendered(Phase phase) { _lastDrawnPhase = static_cast<uint8_t>(phase); }
    uint32_t elapsedMs(uint32_t nowMs) const { return nowMs - _startedMs; }

    static const char* title(Phase phase) {
        switch (phase) {
            case Phase::Hold: return "A: HOLD";
            case Phase::Snapshot: return "B: SNAPSHOT";
            case Phase::Render: return "C: RENDER";
            case Phase::Normal: return nullptr;
        }
        return nullptr;
    }

private:
    uint32_t _startedMs;
    uint8_t _lastDrawnPhase = 0xff;
    bool _complete = false;
};

// Override only the title seen by one render call. Successful and failed
// renders both restore the original RAM snapshot; no configuration is saved.
template <size_t N>
class ScreenIsolationTitle {
public:
    ScreenIsolationTitle(char* title, const char* replacement)
        : _title(title && replacement ? title : nullptr) {
        static_assert(N > 0, "title buffer must not be empty");
        if (!_title) return;
        memcpy(_original, _title, N);
        const size_t length = strlen(replacement);
        const size_t copied = length < N - 1 ? length : N - 1;
        memcpy(_title, replacement, copied);
        _title[copied] = '\0';
    }
    ~ScreenIsolationTitle() {
        if (_title) memcpy(_title, _original, N);
    }
    ScreenIsolationTitle(const ScreenIsolationTitle&) = delete;
    ScreenIsolationTitle& operator=(const ScreenIsolationTitle&) = delete;
private:
    char* _title;
    char _original[N]{};
};

} }  // namespace FastBee::Screen

