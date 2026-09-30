#pragma once
#include "core/MotorTravelGuard.h"
#include <cstddef>
#include <cstdint>

namespace FastBee {

// Per-axis, volatile admission state. Call under the same lock as the bus
// transaction. Persist configuration only, never reference/idle certainty.
struct MotorTravelMapping {
    bool required = false, valid = false, finite = false;
    uint8_t slave = 0, readFunction = 0, writeFunction = 0;
    uint16_t regs[5]{}, values[3]{}, pulseReadRegister = 0;
    int32_t minimum = 0, maximum = 0;

    bool operator==(const MotorTravelMapping& rhs) const {
        if (required != rhs.required || valid != rhs.valid || finite != rhs.finite ||
            slave != rhs.slave || readFunction != rhs.readFunction || writeFunction != rhs.writeFunction ||
            pulseReadRegister != rhs.pulseReadRegister || minimum != rhs.minimum || maximum != rhs.maximum) return false;
        for (unsigned i = 0; i < 5; ++i) if (regs[i] != rhs.regs[i]) return false;
        for (unsigned i = 0; i < 3; ++i) if (values[i] != rhs.values[i]) return false;
        return true;
    }

    bool usable() const {
        if (!valid || !finite || slave < 1 || slave > 247 || maximum <= minimum ||
            (readFunction != 3 && readFunction != 4) || (writeFunction != 6 && writeFunction != 16)) return false;
        for (unsigned i = 0; i < 3; ++i) {
            if (regs[i] == regs[3] || regs[i] == regs[4]) return false;
            for (unsigned j = i + 1; j < 3; ++j) if (regs[i] == regs[j] && values[i] == values[j]) return false;
        }
        return regs[3] != regs[4];
    }
};

struct MotorTravelAdmission {
    MotorTravelMapping mapping;
    MotorTravelGuard position;
    bool stationary = false, pulseKnown = false;
    uint16_t pulse = 0;
    uint32_t pulseAt = 0;

    static uint16_t word(const uint8_t* bytes) {
        return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
    }

    void invalidate() { position.invalidate(); stationary = false; pulseKnown = false; }

    void configure(const MotorTravelMapping& next) {
        if (!(mapping == next)) { invalidate(); mapping = next; }
        position.configure(mapping.minimum, mapping.maximum);
    }

    bool confirmPosition(int32_t measuredPosition, bool operatorConfirmedStationary) {
        invalidate();
        if (!mapping.required || !mapping.usable() || !operatorConfirmedStationary ||
            !position.confirm(measuredPosition)) return false;
        stationary = true;
        return true;
    }

    // An operator's stationary confirmation cannot narrow the possible-position
    // interval, recreate a lost reference, or establish parameter readback.
    bool confirmStationary(bool confirmed) {
        if (!confirmed || !mapping.usable() || !position.referenced) return false;
        stationary = true;
        return true;
    }

    const char* checkStart(bool forward, uint16_t requested, uint32_t now) const {
        if (!mapping.usable()) return "TRAVEL_CALIBRATION_REQUIRED";
        if (!position.referenced) return "TRAVEL_REFERENCE_REQUIRED";
        if (!stationary) return "TRAVEL_STATIONARY_CONFIRMATION_REQUIRED";
        if (!pulseKnown || static_cast<uint32_t>(now - pulseAt) > 15000U || requested != pulse)
            return "TRAVEL_FRESH_PULSE_READBACK_REQUIRED";
        if (!requested || position.available(forward, requested) != requested) return "TRAVEL_RANGE_EXCEEDED";
        return nullptr;
    }

    // Full RTU request including CRC. The transport must validate the CRC
    // before calling this method. Unknown writes, broadcasts and multi-register
    // writes fail closed for a protected axis; unrelated slaves are unaffected.
    const char* before(const uint8_t* frame, size_t length, uint32_t now) {
        if (!mapping.required) return nullptr;
        if (!frame || length < 4) return "TRAVEL_INVALID_FRAME";
        if (frame[0] != 0 && frame[0] != mapping.slave) return nullptr;
        if (frame[0] == 0) return "TRAVEL_BROADCAST_BLOCKED";
        const uint8_t fc = frame[1];
        if (fc >= 1 && fc <= 4 && length == 8) {
            const uint32_t count = word(frame + 4), first = word(frame + 2);
            if (count && count <= (fc <= 2 ? 2000U : 125U) && first + count <= 65536U) return nullptr;
        }
        uint16_t reg = 0, value = 0;
        if (fc == 6 && length == 8) { reg = word(frame + 2); value = word(frame + 4); }
        else if (fc == 16 && length == 11 && word(frame + 4) == 1 && frame[6] == 2) {
            reg = word(frame + 2); value = word(frame + 7);
        } else return "TRAVEL_WRITE_BLOCKED";
        if (fc != mapping.writeFunction || !mapping.valid) return "TRAVEL_PROTOCOL_INVALID";
        // Stop must be unambiguous even for an invalid legacy register map.
        bool stop = reg == mapping.regs[2] && value == mapping.values[2];
        for (unsigned i = 0; i < 2; ++i)
            if (reg == mapping.regs[i] && value == mapping.values[i]) stop = false;
        if (reg == mapping.regs[3] || reg == mapping.regs[4]) stop = false;
        if (stop) { invalidate(); return nullptr; }
        if (!mapping.usable()) return "TRAVEL_CALIBRATION_REQUIRED";
        if (!position.referenced) return "TRAVEL_REFERENCE_REQUIRED";
        if (!stationary) return "TRAVEL_STATIONARY_CONFIRMATION_REQUIRED";
        if (reg == mapping.regs[3] || reg == mapping.regs[4]) {
            pulseKnown = false;
            return nullptr;
        }
        int direction = -1;
        for (unsigned i = 0; i < 2; ++i)
            if (reg == mapping.regs[i] && value == mapping.values[i]) direction = static_cast<int>(i);
        if (direction < 0) return "TRAVEL_WRITE_BLOCKED";
        if (const char* error = checkStart(direction == 0, pulse, now)) return error;
        if (!position.reserve(direction == 0, pulse)) return "TRAVEL_RANGE_EXCEEDED";
        stationary = false;
        return nullptr;
    }

    // Transport-validated CRC/address is necessary but not sufficient: check
    // exact read shape or write echo before trusting any acknowledgement.
    void after(const uint8_t* request, size_t requestLength, const uint8_t* reply,
               size_t replyLength, bool transportSuccess, uint32_t now) {
        if (!mapping.required || !request || requestLength < 8 || request[0] != mapping.slave) return;
        const uint8_t fc = request[1];
        if (fc >= 1 && fc <= 4) {
            if (!transportSuccess) { invalidate(); return; }
            if (fc != mapping.readFunction || word(request + 2) != mapping.pulseReadRegister || word(request + 4) != 1) return;
            pulseKnown = transportSuccess && reply && replyLength == 7 && reply[0] == mapping.slave && reply[1] == fc && reply[2] == 2;
            if (pulseKnown) { pulse = word(reply + 3); pulseAt = now; }
            else invalidate();
            return;
        }
        bool echo = transportSuccess && reply && replyLength == 8;
        if (echo) for (unsigned i = 0; i < 6; ++i) if (reply[i] != request[i]) echo = false;
        if (!echo) invalidate();
    }
};

} // namespace FastBee
