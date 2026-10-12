#pragma once
#include "core/MotorTravelAdmission.h"
#include <array>

namespace FastBee {

// Owned by one bus. Every method runs under its transaction semaphore.
template <size_t Capacity> class MotorTravelInterlock {
public:
    std::array<MotorTravelAdmission, Capacity> axes{};
    uint32_t generation = 1;

    void reset() {
        for (auto& axis : axes) axis.invalidate();
        ++generation;
    }

    void configure(size_t index, const MotorTravelMapping& mapping) {
        if (index >= Capacity) return;
        if (!(axes[index].mapping == mapping)) ++generation;
        axes[index].configure(mapping);
    }

    bool protects(uint8_t slave) const {
        for (const auto& axis : axes)
            if (axis.mapping.required && (!slave || axis.mapping.slave == slave)) return true;
        return false;
    }

    const char* before(const uint8_t* frame, size_t length, uint32_t now) {
        if (!frame || length < 4) return "TRAVEL_INVALID_FRAME";
        size_t matches = 0, index = 0;
        for (size_t i = 0; i < Capacity; ++i) {
            if (!axes[i].mapping.required) continue;
            if (!frame[0]) return "TRAVEL_BROADCAST_BLOCKED";
            if (axes[i].mapping.slave == frame[0]) { ++matches; index = i; }
        }
        if (!matches) return nullptr;
        if (matches != 1) return "TRAVEL_DUPLICATE_SLAVE";
        const char* error = axes[index].before(frame, length, now);
        if (!error && (frame[1] < 1 || frame[1] > 4)) ++generation;
        return error;
    }

    void after(const uint8_t* request, size_t requestLength, const uint8_t* reply,
               size_t replyLength, bool success, uint32_t now) {
        for (auto& axis : axes) {
            const bool referenced = axis.position.referenced, stationary = axis.stationary, known = axis.pulseKnown;
            const bool stopped = axis.stopAcknowledged;
            const uint16_t pulse = axis.pulse;
            axis.after(request, requestLength, reply, replyLength, success, now);
            if (referenced != axis.position.referenced || stationary != axis.stationary ||
                known != axis.pulseKnown || pulse != axis.pulse || stopped != axis.stopAcknowledged) ++generation;
        }
    }

    const char* confirm(size_t index, uint32_t expectedGeneration, bool reference,
                        int32_t position, bool stationary) {
        if (index >= Capacity || !axes[index].mapping.required) return "TRAVEL_AXIS_UNAVAILABLE";
        if (expectedGeneration != generation) return "TRAVEL_STATE_CHANGED";
        if (!stationary) return "TRAVEL_STATIONARY_CONFIRMATION_REQUIRED";
        auto& axis = axes[index];
        const bool accepted = reference ? axis.confirmPosition(position, stationary)
                                        : axis.confirmStationary(stationary);
        ++generation;
        if (!accepted) return reference ? "TRAVEL_REFERENCE_REJECTED" : "TRAVEL_REFERENCE_REQUIRED";
        // Confirmation never establishes fresh parameter readback.
        axis.pulseKnown = false;
        return nullptr;
    }
};

} // namespace FastBee
