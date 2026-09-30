#pragma once
#include "core/PeripheralExecution.h"

namespace {
// Event data-source triggers store their binding in eventId, while platform and
// polling triggers use triggerPeriphId. Either retired binding pauses the whole
// rule, including its other OR triggers and otherwise valid local actions.
inline bool hasRetiredRemoteSource(const ExecTrigger& trigger) {
    return trigger.triggerPeriphId.startsWith("remote.") ||
           trigger.eventId.startsWith("ds:remote.");
}

} // namespace
