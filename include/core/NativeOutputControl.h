#pragma once
#include "core/OrdinaryControlPolicy.h"
#include "core/UartEventOrigin.h"

namespace FastBee {
enum class DigitalOutputCommand : uint8_t { Low, High, Toggle, Blink, Stop };
enum class ScalarOutputCommand : uint8_t { Pwm, Dac, Breathe, Stop, Toggle, Blink };
struct OrdinaryControlContext {
    OrdinaryControl::Request request;
    uint32_t configEpoch = 0;
    uint32_t ruleEpoch = 0; // Zero only for direct non-rule output.
    uint32_t operationDeadlineAt = 0; // Existing worker millis deadline; zero means none, never a client budget.
    bool ruleBound = false;
    bool modbusMaintenance = false; // Issued only by the authenticated HTTP maintenance boundary.
    char reviewedTargetId[41]{};
    UartEventOrigin uartOrigin;
};
struct OrdinaryControlResult {
    char targetId[41]{};
    OrdinaryControl::Decision decision = OrdinaryControl::Decision::Invalid;
    OrdinaryControl::Actor actor;
    OrdinaryControl::Phase phase = OrdinaryControl::Phase::Idle;
    uint64_t operation = 0;
    uint64_t generation = 0;
    uint32_t holdRemainingMs = 0;
    bool attempted = false;
    bool attemptKnown = true; // False while an accepted asynchronous worker has no terminal result.
    uint32_t backendOperationId = 0; // Bounded worker result retaining the complete output prefix.
    uint16_t acknowledgedSteps = 0; // Transport ACKs only; never physical feedback.
    bool evaluated = false;
    bool accepted = false;
    bool cleanupConfirmed = false;
    const char* reason = "CONTROL_INVALID";
};
} // namespace FastBee

