#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstddef>
#include <atomic>
#include <new>
#include <climits>

namespace FastBee {

// Reply storage exists only after a usable OTA command arrives. Readers retain
// all three queues across callbacks; retirement never drops queued replies.
class OtaReplyQueueOwner {
    struct State {
        std::atomic<unsigned> references{1}; // Owner plus admitted callers.
        QueueHandle_t terminal = nullptr, replies = nullptr, progress = nullptr;
        ~State() {
            if (progress) vQueueDelete(progress);
            if (replies) vQueueDelete(replies);
            if (terminal) vQueueDelete(terminal);
        }
        static void release(State* state) {
            if (state && state->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete state;
        }
    };
public:
    class Lease {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : state_(other.state_) { other.state_ = nullptr; }
        ~Lease() { State::release(state_); }
        explicit operator bool() const { return state_ != nullptr; }
        QueueHandle_t terminal() const { return state_ ? state_->terminal : nullptr; }
        QueueHandle_t replies() const { return state_ ? state_->replies : nullptr; }
        QueueHandle_t progress() const { return state_ ? state_->progress : nullptr; }
    private:
        friend class OtaReplyQueueOwner;
        explicit Lease(State* state) : state_(state) {}
        State* state_ = nullptr;
    };

    explicit OtaReplyQueueOwner(size_t itemSize) : itemSize_(itemSize) {}
    OtaReplyQueueOwner(const OtaReplyQueueOwner&) = delete;
    OtaReplyQueueOwner& operator=(const OtaReplyQueueOwner&) = delete;
    ~OtaReplyQueueOwner() {
        // Already-admitted callers retain storage even if the owner retires.
        portENTER_CRITICAL(&mux_);
        State* state = state_;
        state_ = nullptr;
        portEXIT_CRITICAL(&mux_);
        State::release(state);
    }

    bool ensure() {
        return static_cast<bool>(ensureAndAcquire());
    }

    Lease ensureAndAcquire() {
        portENTER_CRITICAL(&mux_);
        if (changing_) { portEXIT_CRITICAL(&mux_); return {}; }
        if (state_) {
            if (state_->references.load(std::memory_order_acquire) == UINT_MAX) {
                portEXIT_CRITICAL(&mux_); return {};
            }
            state_->references.fetch_add(1, std::memory_order_relaxed);
            Lease lease(state_);
            portEXIT_CRITICAL(&mux_);
            return lease;
        }
        changing_ = true;
        portEXIT_CRITICAL(&mux_);
        // Allocators and queue deletion never run inside a critical section.
        State* candidate = new (std::nothrow) State;
        if (candidate) {
            candidate->terminal = xQueueCreate(1, itemSize_);
            if (candidate->terminal) candidate->replies = xQueueCreate(8, itemSize_);
            if (candidate->replies) candidate->progress = xQueueCreate(1, itemSize_);
        }
        const bool complete = candidate && candidate->terminal && candidate->replies && candidate->progress;
        if (!complete) State::release(candidate);
        portENTER_CRITICAL(&mux_);
        if (complete) {
            // Publish storage together with the command's lease. Idle cleanup
            // cannot retire it between successful creation and task admission.
            candidate->references.fetch_add(1, std::memory_order_relaxed);
            state_ = candidate;
        }
        changing_ = false;
        portEXIT_CRITICAL(&mux_);
        return complete ? Lease(candidate) : Lease();
    }

    Lease acquire() {
        portENTER_CRITICAL(&mux_);
        if (changing_ || !state_ || state_->references.load(std::memory_order_acquire) == UINT_MAX) {
            portEXIT_CRITICAL(&mux_);
            return {};
        }
        state_->references.fetch_add(1, std::memory_order_relaxed);
        Lease lease(state_);
        portEXIT_CRITICAL(&mux_);
        return lease;
    }

    template <typename Idle>
    bool releaseIfIdle(Idle idle) {
        portENTER_CRITICAL(&mux_);
        if (changing_) { portEXIT_CRITICAL(&mux_); return false; }
        if (!state_) { portEXIT_CRITICAL(&mux_); return true; }
        if (state_->references.load(std::memory_order_acquire) != 1) {
            portEXIT_CRITICAL(&mux_); return false;
        }
        // No queue caller exists while the owner is the sole reference. Only nonblocking queue-count
        // and owner-state reads are permitted in this predicate.
        if (uxQueueMessagesWaiting(state_->terminal) || uxQueueMessagesWaiting(state_->replies) ||
            uxQueueMessagesWaiting(state_->progress) || !idle()) {
            portEXIT_CRITICAL(&mux_);
            return false;
        }
        changing_ = true;
        State* state = state_;
        state_ = nullptr;
        portEXIT_CRITICAL(&mux_);
        State::release(state);
        portENTER_CRITICAL(&mux_);
        changing_ = false;
        portEXIT_CRITICAL(&mux_);
        return true;
    }
private:
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    const size_t itemSize_;
    State* state_ = nullptr;
    bool changing_ = false;
};

} // namespace FastBee
