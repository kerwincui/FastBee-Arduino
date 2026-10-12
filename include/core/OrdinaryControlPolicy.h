#pragma once

#include <stdint.h>

// All state below belongs to PeripheralRuntimeState and is read/written under
// PeripheralManager's existing recursive mutex. There is no resource registry.
namespace FastBee {
namespace OrdinaryControl {

enum class Source : uint8_t { None, Rule, Mqtt, Http, Screen };
enum class Phase : uint8_t { Idle, Executing, Held, Producing, Retiring, Fault };
enum class Intent : uint8_t { Write, ReplaceOwnedProducer, Stop };

struct Actor {
    Source source = Source::None;
    uint64_t identity = 0; // Server-issued, boot-scoped; never a client value.
};

inline bool sameActor(const Actor& a, const Actor& b) {
    return a.source == b.source && a.identity == b.identity;
}
inline bool isManual(Source source) {
    return source == Source::Http || source == Source::Screen;
}
inline uint8_t priority(Source source) {
    return isManual(source) ? 2 : source == Source::None ? 0 : 1;
}
inline bool validSource(Source source) {
    return source == Source::Rule || source == Source::Mqtt ||
           source == Source::Http || source == Source::Screen;
}

constexpr uint32_t DefaultManualHoldMs = 30000;
constexpr uint32_t MaximumHoldMs = 300000;

struct Request {
    Actor actor;
    uint64_t operation = 0; // PM-issued monotonic number; exhaustion refuses.
    Intent intent = Intent::Write;
    uint32_t holdMs = 0; // Boundary assigns manual default; not inferred here.
    bool overrideConfirmed = false; // Set only after server validates review.
    uint64_t reviewedGeneration = 0;
};

struct Lease {
    Actor actor;
    uint64_t operation = 0;
    uint64_t generation = 0; // Unique grant; separate from operation identity.
    uint64_t retiredThrough = 0; // Requests at/below this watermark never run.
    uint64_t acquiredMs = 0; // esp_timer_get_time()/1000, boot-local monotonic.
    uint32_t holdMs = 0;
    Phase phase = Phase::Idle;
    bool invocationActive = false; // Cleared only by terminal acknowledgement or config teardown.
};

struct Permit {
    Actor actor;
    uint64_t operation = 0;
    uint64_t generation = 0;
};

// A positive retirement decision is NOT permission to execute the new output.
enum class Decision : uint8_t {
    Acquire,
    Continue,
    StopIdle,
    RetireOwned,
    RetireForOverride,
    RetireExpired,
    Invalid,
    StaleOperation,
    DedicatedOwner,
    CleanupPending,
    Conflict,
    StaleReview,
    PriorityDenied
};

inline bool expired(const Lease& lease, uint64_t nowMs) {
    if (lease.phase == Phase::Idle) return false;
    // A reversed boot-local clock invalidates automatic producers too.
    return nowMs < lease.acquiredMs ||
           (lease.holdMs != 0 && nowMs - lease.acquiredMs >= lease.holdMs);
}

// Admission only. PM must recheck config/version/source/driver readiness under
// the SAME lock, and retain it through bounded synchronous I/O or publish a
// provider-owned in-flight reservation before dropping it for a blocking call.
inline Decision inspect(const Lease& lease, const Request& request,
                        uint64_t nowMs, bool dedicatedOwner) {
    if (!validSource(request.actor.source) || !request.actor.identity ||
        !request.operation || request.holdMs > MaximumHoldMs ||
        (!isManual(request.actor.source) && request.holdMs != 0) ||
        (isManual(request.actor.source) && request.holdMs == 0) ||
        static_cast<uint8_t>(request.intent) > static_cast<uint8_t>(Intent::Stop) ||
        (request.overrideConfirmed &&
         (!isManual(request.actor.source) || !request.reviewedGeneration))) {
        return Decision::Invalid;
    }
    // Dedicated ControlOutput never participates in ordinary priority.
    if (dedicatedOwner) return Decision::DedicatedOwner;
    if (request.operation <= lease.retiredThrough) return Decision::StaleOperation;
    if (lease.phase == Phase::Retiring || lease.phase == Phase::Fault) {
        return Decision::CleanupPending;
    }
    if (lease.phase == Phase::Idle) {
        // A review for a retired/replaced owner must be refreshed, even if idle.
        if (request.overrideConfirmed || request.reviewedGeneration) return Decision::StaleReview;
        return request.intent == Intent::Stop ? Decision::StopIdle : Decision::Acquire;
    }
    if (expired(lease, nowMs)) return Decision::RetireExpired;
    if (sameActor(lease.actor, request.actor)) {
        if (request.operation < lease.operation) return Decision::StaleOperation;
        if (request.operation == lease.operation) {
            // Continuation is exclusively an executing invocation. A retained
            // producer must use its exact Permit, never re-admit the old request.
            return lease.phase == Phase::Executing ? Decision::Continue : Decision::StaleOperation;
        }
        if (lease.phase == Phase::Executing) return Decision::Conflict;
        // A new action by the same actor may replace its own held/ticker output,
        // but the old producer must first confirm retirement. This is not a
        // second actor's override and must not carry a stale takeover review.
        if (request.overrideConfirmed || request.reviewedGeneration) return Decision::StaleReview;
        return Decision::RetireOwned;
    }
    if (!request.overrideConfirmed) return Decision::Conflict;
    if (request.reviewedGeneration != lease.generation) return Decision::StaleReview;
    if (!isManual(request.actor.source) ||
        priority(request.actor.source) <= priority(lease.actor.source)) {
        return Decision::PriorityDenied;
    }
    // Do not interrupt an in-progress multi-action invocation midway through
    // a driver operation. Its next boundary can observe retirement, but this
    // request must retry explicitly after terminal acknowledgement.
    if (lease.phase == Phase::Executing) return Decision::Conflict;
    return Decision::RetireForOverride;
}

inline bool permitCurrent(const Lease& lease, const Permit& permit, uint64_t nowMs) {
    if (!permit.operation || !permit.generation || !sameActor(lease.actor, permit.actor) ||
        lease.operation != permit.operation || lease.generation != permit.generation) return false;
    return (lease.phase == Phase::Executing || lease.phase == Phase::Producing) &&
           !expired(lease, nowMs);
}

} // namespace OrdinaryControl
} // namespace FastBee

