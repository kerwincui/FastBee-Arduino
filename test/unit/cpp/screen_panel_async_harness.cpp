#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "core/NativeOutputControl.h"

#include "panel_async_definitions.inc"

struct Operation {
    uint32_t operationId;
    AsyncExecStatus status;
    AsyncExecError errorCode;
    FastBee::OrdinaryControlResult ordinaryControlResult{};
};

struct Card {
    uint32_t operationId = 42;
    bool pending = false;
    char error[96] = {};
};

void updateCard(Card& cardOut, const std::array<Operation, 2>& recentOperations) {
#include "panel_async_loop.inc"
}

int main() {
    const char* expectedErrors[] = {
        "none", "low_memory", "deadline_exceeded", "action_failed",
        "incomplete_actions", "cancelled_by_system", "worker_queue_full",
        "uart_source_unconfirmed", "rule_revision_unconfirmed"
    };
    for (unsigned value = 0; value <= 255; ++value) {
        Card card;
        card.pending = true;
        const std::array<Operation, 2> operations = {{
            {99, AsyncExecStatus::COMPLETED, AsyncExecError::NONE},
            {42, AsyncExecStatus::FAILED, static_cast<AsyncExecError>(value)}
        }};
        updateCard(card, operations);
        assert(!card.pending);
        assert(std::strcmp(card.error, value < std::size(expectedErrors) ? expectedErrors[value] : "unknown") == 0);
    }

    const AsyncExecStatus statuses[] = {
        AsyncExecStatus::PENDING, AsyncExecStatus::RUNNING, AsyncExecStatus::COMPLETED,
        AsyncExecStatus::FAILED, AsyncExecStatus::CANCELLED, AsyncExecStatus::REJECTED,
        AsyncExecStatus::TIMED_OUT, static_cast<AsyncExecStatus>(255)
    };
    for (unsigned index = 0; index < std::size(statuses); ++index) {
        Card card;
        const std::array<Operation, 2> operations = {{
            {99, AsyncExecStatus::FAILED, AsyncExecError::LOW_MEMORY},
            {42, statuses[index], AsyncExecError::ACTION_FAILED}
        }};
        updateCard(card, operations);
        assert(card.pending == (index < 2 || index == 7));
        assert(std::strcmp(card.error, index >= 3 && index <= 6 ? "action_failed" : "") == 0);
    }

    // The production result accumulator must keep the first native rejection,
    // and this real snapshot loop must display it rather than ACTION_FAILED.
    FastBee::OrdinaryControlResult aggregate, rejected, laterSuccess, laterFailure;
    rejected.evaluated = true;
    rejected.reason = "CONTROL_CONFLICT";
    laterSuccess.evaluated = true;
    laterSuccess.accepted = true;
    laterSuccess.reason = "CONTROL_ACCEPTED_SDK_ONLY";
    laterFailure.evaluated = true;
    laterFailure.reason = "CONTROL_OUTPUT_UNCONFIRMED";
    FastBee::rememberOrdinaryControlResult(rejected, &aggregate);
    FastBee::rememberOrdinaryControlResult(laterSuccess, &aggregate);
    FastBee::rememberOrdinaryControlResult(laterFailure, &aggregate);
    assert(aggregate.evaluated && !aggregate.accepted);
    assert(std::strcmp(aggregate.reason, "CONTROL_CONFLICT") == 0);
    Card conflict;
    conflict.pending = true;
    std::array<Operation, 2> rejectedOperations = {{
        {99, AsyncExecStatus::COMPLETED, AsyncExecError::NONE},
        {42, AsyncExecStatus::FAILED, AsyncExecError::ACTION_FAILED, aggregate}
    }};
    updateCard(conflict, rejectedOperations);
    assert(!conflict.pending && std::strcmp(conflict.error, "CONTROL_CONFLICT") == 0);
    // Unobserved or missing native reasons retain the generic failure; they
    // must never clear the error or convert a failed operation to success.
    for (bool evaluated : {false, true}) {
        Card fallback;
        rejectedOperations[1].ordinaryControlResult.evaluated = evaluated;
        rejectedOperations[1].ordinaryControlResult.reason = evaluated ? nullptr : "CONTROL_CONFLICT";
        updateCard(fallback, rejectedOperations);
        assert(!fallback.pending && std::strcmp(fallback.error, "action_failed") == 0);
    }

    Card unmatched;
    unmatched.pending = true;
    std::strcpy(unmatched.error, "previous");
    const std::array<Operation, 2> otherOperations = {{
        {98, AsyncExecStatus::COMPLETED, AsyncExecError::NONE},
        {99, AsyncExecStatus::FAILED, AsyncExecError::LOW_MEMORY}
    }};
    updateCard(unmatched, otherOperations);
    assert(unmatched.pending);
    assert(std::strcmp(unmatched.error, "previous") == 0);
}
