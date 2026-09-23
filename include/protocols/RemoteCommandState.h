#pragma once

#include <cstdint>

namespace FastBee {
// Transport-independent command lifecycle used by the MQTT adapter and tests.
// Acceptance never substitutes for a fresh readback of a switch's state.
struct RemoteCommandState {
    uint32_t queuedMs = 0;
    uint32_t sentMs = 0;
    bool queued = false;
    bool waiting = false;
    bool target = false;
    const char* result = "";

    bool enqueue(bool desired, bool connected, uint32_t now) {
        if (!connected || queued || waiting) return false;
        target = desired;
        queuedMs = now;
        queued = true;
        result = "queued";
        return true;
    }

    void disconnect() {
        if (queued || waiting) result = "connection_lost";
        queued = false;
        waiting = false;
    }

    bool takeForSend(bool action, bool paused, uint32_t now) {
        if (!queued) return false;
        queued = false;
        if (paused || static_cast<uint32_t>(now - queuedMs) > 2000U) {
            result = "expired_before_send";
            return false;
        }
        sentMs = now;
        waiting = !action;
        result = action ? "sending" : "waiting_for_state";
        return true;
    }

    void published(bool success, bool action) {
        if (!success) {
            waiting = false;
            result = "send_failed";
        } else if (action) {
            result = "sent_unconfirmed";
        }
    }

    void receiveState(bool observed) {
        if (waiting && observed == target) {
            waiting = false;
            result = "confirmed";
        }
    }

    void checkTimeout(uint32_t now, uint32_t timeoutMs) {
        if (waiting && static_cast<uint32_t>(now - sentMs) > timeoutMs) {
            waiting = false;
            result = "result_unknown";
        }
    }
};
} // namespace FastBee
