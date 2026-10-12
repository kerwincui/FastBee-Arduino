#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include "core/UartEventOrigin.h"
#include "core/ModbusOrdinaryControl.h"

// Platform-independent queue policy. The service owns the lock and executes
// only the selected entry; keeping this policy separate allows real host tests.
namespace MotorCommands {
enum class Phase : uint8_t { EMPTY, QUEUED, RUNNING, DONE };
// Admission of the acknowledgement report, independent of command success.
// Retained/queued are local outcomes, never broker delivery confirmation.
enum class ReportState : uint8_t { NotApplicable, Pending, NotProduced, Disabled,
    Queued, Retained, Dropped, InvalidPayload };
inline const char* reportStateName(ReportState state) {
    switch (state) {
        case ReportState::NotApplicable: return "not_applicable";
        case ReportState::Pending: return "pending";
        case ReportState::NotProduced: return "not_produced";
        case ReportState::Disabled: return "disabled";
        case ReportState::Queued: return "queued_locally";
        case ReportState::Retained: return "retained_locally";
        case ReportState::Dropped: return "not_retained";
        case ReportState::InvalidPayload: return "invalid_payload";
    }
    return "unknown";
}
struct OutputStep {
    uint16_t address = 0, value = 0;
    uint8_t function = 0, error = 0;
    bool attempted = false, acknowledged = false;
};
struct Command {
    char ref[48]{}, action[16]{}, requestId[64]{}, error[128]{};
    uint32_t id = 0, order = 0, finishedMs = 0;
    uint32_t deadlineMs = 0;
    uint16_t timeoutMs = 0;
    int value = 0;
    uint16_t speed = 0, pulse = 0;
    Phase phase = Phase::EMPTY;
    bool success = false, known = false;
    bool legacyValue = false;
    FastBee::UartEventOrigin uartOrigin;
    FastBee::OrdinaryControlContext control;
    FastBee::ModbusControlTarget target;
    uint64_t workGeneration = 0;
    static constexpr uint8_t maxOutputSteps = 8;
    std::array<OutputStep, maxOutputSteps> outputSteps{};
    uint8_t outputStepCount = 0;
    bool terminalAcknowledged = false;
    char mqttReportId[48]{};
    uint16_t mqttRequestedValue = 0;
    uint32_t reportInstanceId = 0;
    ReportState reportState = ReportState::NotApplicable;
};
struct Queue {
    static constexpr size_t normalSlots = 16;
    static constexpr size_t stopSlots = 8;
    static constexpr size_t readBegin = normalSlots + stopSlots;
    // Keep read results longer than the Web client's 20 s polling deadline.
    // Separate slots prevent retained reads from displacing control/stop.
    static constexpr size_t readSlots = 48;
    static constexpr uint32_t resultRetentionMs = 30000U;
    std::array<Command, readBegin + readSlots> commands{};
    uint32_t nextId = 0, nextOrder = 0;

    // Returns a slot for a new request, or an existing operation ID for a
    // replay. Replays are checked before busy state, regardless of slot order.
    bool reserve(const char* ref, const char* action, int value, const char* request,
                 uint32_t now, int& slot, uint32_t& existing, const char*& error,
                 const FastBee::UartEventOrigin& uartOrigin = {},
                 const FastBee::OrdinaryControlContext* control = nullptr,
                 const FastBee::ModbusControlTarget* target = nullptr) {
        slot = -1; existing = 0;
        auto sameOrigin = [&](const Command& c) {
            const FastBee::OrdinaryControl::Actor actor = control ? control->request.actor : FastBee::OrdinaryControl::Actor{};
            return FastBee::OrdinaryControl::sameActor(c.control.request.actor, actor) &&
                (!target || c.target.sameMapping(*target)) &&
                c.uartOrigin.required == uartOrigin.required &&
                c.uartOrigin.configEpoch == uartOrigin.configEpoch &&
                c.uartOrigin.receiveGeneration == uartOrigin.receiveGeneration;
        };
        const bool stop = !std::strcmp(action, "stop"), read = !std::strcmp(action, "readStatus");
        for (auto& c : commands) {
            if (c.phase == Phase::DONE && (!c.workGeneration || c.terminalAcknowledged) && now - c.finishedMs >= resultRetentionMs) c = Command{};
            if (c.phase != Phase::EMPTY && request[0] && !std::strcmp(request, c.requestId)) {
                if (std::strcmp(ref, c.ref) || std::strcmp(action, c.action) || value != c.value || !sameOrigin(c)) {
                    error = "Request ID conflicts with another command"; return false;
                }
                existing = c.id; return true;
            }
        }
        for (size_t i = 0; i < commands.size(); ++i) {
            const auto& c = commands[i];
            if (c.phase == Phase::EMPTY) {
                if (slot < 0 && (stop ? i >= normalSlots && i < readBegin :
                                read ? i >= readBegin : i < normalSlots)) slot = static_cast<int>(i);
                continue;
            }
            if (!std::strcmp(ref, c.ref) && (c.phase == Phase::QUEUED || c.phase == Phase::RUNNING ||
                (c.phase == Phase::DONE && c.workGeneration && !c.terminalAcknowledged))) {
                if (!std::strcmp(action, c.action) && (stop || read) && sameOrigin(c)) { existing = c.id; return true; }
                // Background status reads must not reject a user's control.
                // The single worker finishes any in-flight read before the
                // write, while an existing write still excludes another one.
                if (!stop && (read || std::strcmp(c.action, "readStatus"))) {
                    error = "Motor command pending"; return false;
                }
            }
            // A motor always has a stop slot. Replacing its previous completed
            // stop is safe because stop is idempotent; movement results are
            // never evicted before their deduplication deadline.
            if (stop && i >= normalSlots && i < readBegin && !std::strcmp(ref, c.ref) && c.phase == Phase::DONE && (!c.workGeneration || c.terminalAcknowledged))
                slot = static_cast<int>(i);
        }
        if (slot < 0) { error = "Motor command rate limit; retry later"; return false; }
        return true;
    }
    int selectNext() const {
        int selected = -1;
        auto priority = [](const Command& command) {
            return !std::strcmp(command.action, "stop") ? 0 :
                   !std::strcmp(command.action, "readStatus") ? 2 : 1;
        };
        for (size_t i = 0; i < commands.size(); ++i) {
            const auto& c = commands[i];
            if (c.phase != Phase::QUEUED) continue;
            if (selected < 0 ||
                priority(c) < priority(commands[selected]) ||
                (priority(c) == priority(commands[selected]) &&
                 static_cast<int32_t>(c.order - commands[selected].order) < 0)) selected = static_cast<int>(i);
        }
        return selected;
    }
    void cancelQueued(const char* ref, uint32_t now) {
        for (auto& c : commands) if (!std::strcmp(ref, c.ref) && c.phase == Phase::QUEUED) {
            c.phase = Phase::DONE; c.finishedMs = now;
            if (c.reportState == ReportState::Pending) c.reportState = ReportState::NotProduced;
            std::strcpy(c.error, "Cancelled by stop");
        }
    }
    uint32_t allocateId() {
        // HTTP operation IDs are positive signed integers. On wrap, skip any
        // ID still retained by a pending request or the replay cache.
        for (;;) {
            if (nextId >= 0x7ffffffeU) nextId = 0;
            ++nextId;
            bool used = false;
            for (const auto& c : commands) if (c.phase != Phase::EMPTY && c.id == nextId) used = true;
            if (!used) return nextId;
        }
    }
};
}
