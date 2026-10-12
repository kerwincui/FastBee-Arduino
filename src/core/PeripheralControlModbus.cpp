#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "protocols/ModbusHandler.h"
#include <esp_timer.h>
#if FASTBEE_ENABLE_MODBUS
namespace {
namespace OC = FastBee::OrdinaryControl;
using Slot = FastBee::ModbusOrdinarySlot;
using Context = FastBee::OrdinaryControlContext;
using Result = FastBee::OrdinaryControlResult;
bool sameWorkScope(const FastBee::ModbusControlTarget& a, const FastBee::ModbusControlTarget& b) {
    return a.sameMapping(b) || (a.kind==FastBee::ModbusControlTarget::Kind::Frame &&
        b.kind==FastBee::ModbusControlTarget::Kind::Frame && a.instance==b.instance &&
        a.configuration==b.configuration && a.slave==b.slave);
}
bool modbusSlotActive(const Slot& slot) {
    return slot.hardwareUntilMs || slot.inFlight || slot.workerReserved || slot.pendingStopOperation ||
        slot.lease.invocationActive || slot.lease.phase!=OC::Phase::Idle;
}
bool modbusNow(uint64_t& now) {
    const int64_t us = esp_timer_get_time();
    if (us < 0) return false;
    now = static_cast<uint64_t>(us) / 1000ULL; return true;
}
bool sourceEpochsCurrent(const Context& context) {
    return (!context.operationDeadlineAt ||
            static_cast<int32_t>(static_cast<uint32_t>(millis()) - context.operationDeadlineAt) < 0) &&
        FastBee::SensorConfigEpoch::matches(context.configEpoch) &&
        (!context.ruleBound || FastBee::RuleOutputIdentity::matches(context.ruleEpoch)) &&
        (context.request.actor.source != OC::Source::Screen ||
            FastBee::ScreenOutputIdentity::matches(context.request.actor.identity));
}
// Worker validation runs after releasing the motor queue lock. Bound this PM
// wait and preserve the caller's operation deadline; never retry an output.
TickType_t ordinaryWorkerLockWait(const Context& context) {
    uint32_t waitMs = 100;
    if (context.operationDeadlineAt) {
        const int32_t remaining = static_cast<int32_t>(context.operationDeadlineAt -
            static_cast<uint32_t>(millis()));
        if (remaining <= 0) return 0;
        if (static_cast<uint32_t>(remaining) < waitMs) waitMs = static_cast<uint32_t>(remaining);
    }
    return pdMS_TO_TICKS(waitMs);
}
bool ordinaryWorkerDeadlineExpired(const Context& context) {
    return context.operationDeadlineAt &&
        static_cast<int32_t>(static_cast<uint32_t>(millis()) - context.operationDeadlineAt) >= 0;
}
void fillModbusResult(const Slot& slot, uint64_t now, Result& result) {
    slot.target.key(result.targetId);
    result.actor = slot.lease.actor; result.operation = slot.lease.operation;
    result.generation = slot.lease.generation; result.phase = slot.lease.phase;
    result.holdRemainingMs = slot.lease.holdMs && now >= slot.lease.acquiredMs &&
        now - slot.lease.acquiredMs < slot.lease.holdMs ?
        slot.lease.holdMs - static_cast<uint32_t>(now - slot.lease.acquiredMs) : 0;
}
void retireModbus(Slot& slot, uint64_t cutoff) {
    slot.gate.fetch_or(3U, std::memory_order_acq_rel);
    if (slot.pendingStopOperation && cutoff >= slot.pendingStopOperation) cutoff = slot.pendingStopOperation - 1;
    if (cutoff > slot.lease.retiredThrough) slot.lease.retiredThrough = cutoff;
    slot.lease.phase = OC::Phase::Retiring;
    // Only the actual terminal acknowledgement releases an active invocation.
    if (slot.hardwareUntilMs || slot.inFlight || slot.workerReserved || slot.lease.invocationActive) return;
    const uint64_t watermark = slot.lease.retiredThrough;
    slot.lease = OC::Lease{}; slot.lease.retiredThrough = watermark;
    // Last electrical state and cumulative failure history remain untouched.
}
const char* refused(OC::Decision decision) {
    return decision == OC::Decision::StaleReview ? "CONTROL_REVIEW_STALE" :
        decision == OC::Decision::StaleOperation ? "CONTROL_OPERATION_STALE" :
        decision == OC::Decision::CleanupPending ? "CONTROL_CLEANUP_PENDING" :
        decision == OC::Decision::Invalid ? "CONTROL_INVALID" : "CONTROL_CONFLICT";
}
}

bool PeripheralManager::beginOrdinaryModbusMaintenance(uint64_t actorIdentity,
    uint32_t expectedConfigEpoch, Context& context) {
    // Reuse the issuer's identity/epoch/non-reused operation validation, while
    // keeping its ordinary manual-output policy unchanged.
    if (!beginOrdinaryControl(OC::Source::Http, actorIdentity, OC::DefaultManualHoldMs,
            false, 0, String(), expectedConfigEpoch, FastBee::UartEventOrigin{}, context)) return false;
    context.request.holdMs=0;
    context.modbusMaintenance=true;
    return true;
}

void PeripheralManager::serviceOrdinaryModbusLocked() {
    uint64_t now = 0;
    const bool clock = modbusNow(now);
    for (auto& slot : _ordinaryModbus) {
        const bool hardwareExpired=clock && slot.hardwareUntilMs &&
            slot.hardwareUntilMs!=UINT64_MAX && now>=slot.hardwareUntilMs;
        if(hardwareExpired)slot.hardwareUntilMs=0;
        if (slot.lease.phase == OC::Phase::Idle) continue;
        if (!clock || !sourceEpochsCurrent(slot.context) ||
            !isUartEventOriginCurrent(slot.context.uartOrigin) || OC::expired(slot.lease, now) ||
            slot.lease.phase == OC::Phase::Retiring || slot.lease.phase == OC::Phase::Fault)
            // Expiring the previous physical window cannot revoke the new
            // request whose admission triggered this service pass. Preserve
            // every existing retirement watermark, retiring only its owner.
            retireModbus(slot, hardwareExpired ? slot.lease.operation : _nextOrdinaryControlOperation);
    }
}
void PeripheralManager::finishOrdinaryModbusLocked(const Context& context, bool cancelled) {
    uint64_t now = 0;
    const bool clock = modbusNow(now);
    for (auto& slot : _ordinaryModbus) {
        auto& lease = slot.lease;
        if (slot.pendingStopOperation == context.request.operation &&
            OC::sameActor(slot.pendingStopActor, context.request.actor)) {
            slot.pendingStopTerminal = true; slot.pendingStopCancelled |= cancelled;
        }
        if (!lease.operation || lease.operation != context.request.operation ||
            !OC::sameActor(lease.actor, context.request.actor)) continue;
        if (slot.inFlight || slot.workerReserved) {
            slot.terminalRequested = true; slot.terminalCancelled |= cancelled;
            if (cancelled) retireModbus(slot, _nextOrdinaryControlOperation);
            continue;
        }
        lease.invocationActive = false;
        if (cancelled || !clock || !sourceEpochsCurrent(context) ||
            !isUartEventOriginCurrent(context.uartOrigin) || OC::expired(lease, now) ||
            lease.phase == OC::Phase::Retiring || lease.phase == OC::Phase::Fault || !lease.holdMs)
            retireModbus(slot, cancelled ? _nextOrdinaryControlOperation : lease.operation);
        else if (lease.phase == OC::Phase::Executing) lease.phase = OC::Phase::Held;
    }
}

FastBee::ModbusOrdinarySlot* PeripheralManager::acquireOrdinaryModbusLocked(
    const FastBee::ModbusControlTarget& target, const Context& context, Result& result,
    uint64_t now, uint64_t workerGeneration) {
    auto refuse = [&](const char* reason) -> Slot* { result.reason = reason; return nullptr; };
    Slot* reservation = nullptr;
    serviceOrdinaryModbusLocked();
    if (!sourceEpochsCurrent(context) || !isUartEventOriginCurrent(context.uartOrigin))
        return refuse("CONTROL_SOURCE_STALE");
    char canonical[41]; target.key(canonical);
    if (context.request.overrideConfirmed && std::strcmp(canonical, context.reviewedTargetId))
        return refuse("CONTROL_REVIEW_TARGET_MISMATCH");
    if (!context.request.operation || context.request.operation > _nextOrdinaryControlOperation ||
        context.request.operation <= _ordinaryModbusRetiredFloor) return refuse("CONTROL_OPERATION_STALE");
    const bool maintenance=target.kind==FastBee::ModbusControlTarget::Kind::Maintenance;
    if (maintenance != context.modbusMaintenance ||
        (maintenance && (context.request.actor.source!=OC::Source::Http ||
         context.ruleBound || !context.uartOrigin.unbound() || context.request.overrideConfirmed ||
         context.request.reviewedGeneration || context.reviewedTargetId[0] || context.request.holdMs)))
        return refuse("CONTROL_MAINTENANCE_AUTHORITY_REQUIRED");
    if (!_ordinaryModbus.ensureAllocated()) return refuse("CONTROL_MEMORY_UNAVAILABLE");
    for (const auto& active:_ordinaryModbus) {
        // There is one physical Modbus bus. A changed handler identity cannot
        // cancel a device timer already sent by its predecessor.
        if (active.hardwareUntilMs && (maintenance || active.target.slave==target.slave) &&
            !(active.target.instance==target.instance && active.lease.operation==context.request.operation &&
              OC::sameActor(active.lease.actor,context.request.actor)))
            return refuse("CONTROL_HARDWARE_WINDOW_ACTIVE");
        if (active.target.instance!=target.instance || !modbusSlotActive(active)) continue;
        const bool exclusive=maintenance || active.target.kind==FastBee::ModbusControlTarget::Kind::Maintenance;
        if (exclusive && !(maintenance && active.target.kind==FastBee::ModbusControlTarget::Kind::Maintenance &&
            active.lease.operation==context.request.operation && OC::sameActor(active.lease.actor,context.request.actor)))
            return refuse("CONTROL_MAINTENANCE_BUSY");
    }
    Slot* available = nullptr;
    for (auto& slot : _ordinaryModbus) {
        if (slot.target.instance == target.instance && slot.target.slave == target.slave) {
            reservation = &slot; break;
        }
        if (!available && !slot.hardwareUntilMs && !slot.pendingStopOperation && !slot.workerReserved && !slot.inFlight && !slot.lease.invocationActive &&
            slot.lease.phase == OC::Phase::Idle && slot.sequence < 0x3fffffffU) available = &slot;
    }
    if (!reservation) {
        if (!available) return refuse("CONTROL_CAPACITY");
        reservation = available;
        // Eviction never permits an old finished operation to be replayed
        // against a temporarily absent key. Older unfinished requests may
        // fail closed at bounded-table pressure and must be issued afresh.
        if (reservation->lease.retiredThrough > _ordinaryModbusRetiredFloor)
            _ordinaryModbusRetiredFloor = reservation->lease.retiredThrough;
        if (context.request.operation <= _ordinaryModbusRetiredFloor) return refuse("CONTROL_OPERATION_STALE");
        reservation->target = target;
        reservation->lease.retiredThrough = _ordinaryModbusRetiredFloor;
    }
    auto& slot = *reservation;
    fillModbusResult(slot, now, result);
    if (slot.inFlight) return refuse("CONTROL_TRANSACTION_PENDING");
    if (slot.pendingStopOperation) return refuse("CONTROL_STOP_PENDING");
    if (slot.workerReserved && (!sameWorkScope(slot.target,target) || workerGeneration != slot.workerGeneration ||
        context.request.operation != slot.lease.operation || !OC::sameActor(context.request.actor, slot.lease.actor)))
        return refuse("CONTROL_WORKER_PENDING");
    if (workerGeneration && !slot.workerReserved) return refuse("CONTROL_WORKER_STALE");
    if (slot.target.configuration != target.configuration && slot.lease.phase != OC::Phase::Idle) {
        retireModbus(slot, _nextOrdinaryControlOperation);
        return refuse("CONTROL_TARGET_CHANGED_RETRY_NEW_OPERATION");
    }
    OC::Request request = context.request; request.intent = OC::Intent::Write;
    if (maintenance) {
        // Maintenance never creates a Held lease, replaces an ordinary actor,
        // consumes a manual takeover review or relaxes inspect() for GPIO.
        result.decision = !request.actor.identity || request.operation <= slot.lease.retiredThrough ?
            OC::Decision::StaleOperation :
            slot.lease.phase==OC::Phase::Idle ? OC::Decision::Acquire :
            slot.lease.phase==OC::Phase::Executing && slot.lease.invocationActive &&
                slot.lease.operation==request.operation && OC::sameActor(slot.lease.actor,request.actor) ?
                    OC::Decision::Continue : OC::Decision::Conflict;
    } else result.decision = OC::inspect(slot.lease, request, now, false);
    if (result.decision == OC::Decision::RetireExpired) {
        retireModbus(slot, _nextOrdinaryControlOperation);
        return refuse("CONTROL_EXPIRED_RETRY_NEW_OPERATION");
    }
    if (result.decision == OC::Decision::RetireOwned || result.decision == OC::Decision::RetireForOverride) {
        retireModbus(slot, slot.lease.operation);
        if (slot.lease.phase != OC::Phase::Idle) return refuse("CONTROL_CLEANUP_PENDING");
    } else if (result.decision != OC::Decision::Acquire && result.decision != OC::Decision::Continue)
        return refuse(refused(result.decision));
    if (slot.sequence == 0x3fffffffU || _nextOrdinaryControlGeneration == UINT64_MAX)
        return refuse("CONTROL_CAPACITY");
    if (slot.lease.phase == OC::Phase::Idle) {
        slot.lease.actor = request.actor; slot.lease.operation = request.operation;
        slot.lease.generation = ++_nextOrdinaryControlGeneration;
        slot.lease.acquiredMs = now; slot.lease.holdMs = request.holdMs;
        slot.lease.phase = OC::Phase::Executing; slot.lease.invocationActive = true;
        slot.terminalRequested = false; slot.terminalCancelled = false;
    }
    if (!slot.workerReserved) slot.target = target;
    slot.context = context;
    return reservation;
}

OneShotResult PeripheralManager::submitOrdinaryModbusRelay(ModbusHandler& handler,
    const FastBee::ModbusControlTarget& target, uint8_t function, uint16_t address, uint16_t value,
    const Context& context, Result& result, uint16_t timeout, bool toggle, uint64_t workerGeneration) {
    result = Result{}; result.evaluated = true;
    OneShotResult output; output.error = ONESHOT_CONTROL_REJECTED;
    auto fail = [&](const char* reason) { result.reason = reason; output.controlReason = reason; return output; };
    if (!target.accepts(function, address, value) ||
        (target.kind == FastBee::ModbusControlTarget::Kind::Motor && !workerGeneration))
        return fail("CONTROL_PARAMETER_INVALID");
    FastBee::ModbusWriteTicket ticket;
    Slot* reservation = nullptr;
    {
        RecursiveMutexGuard lock(_mutex, 0);
        uint64_t now = 0;
        if (!lock.isLocked()) return fail("CONTROL_BUSY");
        if (!modbusNow(now)) return fail("CONTROL_CLOCK_UNCONFIRMED");
        reservation = acquireOrdinaryModbusLocked(target, context, result, now, workerGeneration);
        if (!reservation) return fail(result.reason);
        auto& slot = *reservation;
        const auto& request = context.request;
        if (slot.workerEmergencyStop && (toggle || function != target.function ||
            address != target.motorRegisters[2] || value != target.stopValue))
            return fail("CONTROL_STOP_ENDPOINT_MISMATCH");
        ticket.target = target; ticket.context = context;
        ticket.function = function; ticket.address = address; ticket.value = value;
        ticket.toggle = toggle;
        ticket.gate = &slot.gate; ticket.armed = (++slot.sequence << 2U) | 1U;
        ticket.acquiredMs = slot.lease.acquiredMs; ticket.holdMs = slot.lease.holdMs;
        if (context.uartOrigin.required) {
            for (const auto& entry : uartPortById) {
                const uint8_t port = entry.second;
                if (port && port < FastBee::UartPortLease::PORT_COUNT && uartPortLeases[port].owned() &&
                    context.uartOrigin.matches(context.configEpoch, uartReceiveGenerations.current(port))) {
                    ticket.uartGeneration = uartReceiveGenerations.handle(port); break;
                }
            }
            if (!ticket.uartGeneration) { retireModbus(slot, request.operation); return fail("CONTROL_SOURCE_STALE"); }
        }
        slot.inFlight = true;
        slot.gate.store(ticket.armed, std::memory_order_release);
        fillModbusResult(slot, now, result);
    }
    // ProtocolManager lease held by caller pins handler; PM is NOT held while
    // waiting for its bus or a response. Bus never calls back into PM.
    output = handler.writeOrdinaryRelayOnce(target, function, address, value, ticket, timeout, toggle);
    {
        // Terminal acknowledgement is not a zero-wait observation. No bus lock
        // is held and a fixed slot cannot be removed/reassigned while inFlight.
        RecursiveMutexGuard lock(_mutex);
        if (!lock.isLocked()) {
            // If the owner mutex is unavailable even after an unbounded wait,
            // fail closed. Do not free a reservation without serialization.
            ticket.gate->fetch_or(3U, std::memory_order_acq_rel);
            result.attempted = output.outputAttempted;
            result.accepted = false;
            result.reason = "CONTROL_TERMINAL_UNCONFIRMED";
            output.error = ONESHOT_CONTROL_REJECTED; output.controlReason = result.reason;
            return output;
        }
        auto& slot = *reservation;
        slot.gate.fetch_or(3U, std::memory_order_acq_rel);
        slot.inFlight = false;
        result.attempted = output.outputAttempted;
        result.accepted = output.error == ONESHOT_SUCCESS;
        result.reason = output.controlReason ? output.controlReason : "CONTROL_RESULT_UNKNOWN";
        if (!result.accepted) {
            if (slot.failures != UINT32_MAX) ++slot.failures;
            if (!slot.firstFailureOperation) {
                slot.firstFailureOperation = context.request.operation; slot.firstFailure = result.reason;
            }
            retireModbus(slot, context.request.operation);
        }
        if (slot.terminalRequested) {
            const bool cancelled = slot.terminalCancelled;
            slot.terminalRequested = false;
            finishOrdinaryModbusLocked(context, cancelled);
        }
        uint64_t now = 0;
        if (!modbusNow(now) || !sourceEpochsCurrent(context) ||
            !isUartEventOriginCurrent(context.uartOrigin) || OC::expired(slot.lease, now))
            retireModbus(slot, _nextOrdinaryControlOperation);
        fillModbusResult(slot, now, result);
    }
    return output;
}
OneShotResult PeripheralManager::submitOrdinaryModbusFrame(ModbusHandler& handler,
    const FastBee::ModbusControlTarget& target, const FastBee::ModbusOutputFrame& requested,
    const Context& context, Result& result, uint16_t timeout,
    uint8_t* rawResponse, size_t rawCapacity, size_t* rawLength, uint64_t workerGeneration) {
    result={};result.evaluated=true;
    OneShotResult output;output.error=ONESHOT_CONTROL_REJECTED;
    if(rawLength)*rawLength=0;
    auto fail=[&](const char* reason){result.reason=reason;output.controlReason=reason;return output;};
    if(!target.valid() || !requested.valid() ||
        (target.kind!=FastBee::ModbusControlTarget::Kind::Frame &&
         target.kind!=FastBee::ModbusControlTarget::Kind::Maintenance) ||
        ((rawResponse||rawLength||rawCapacity) && (!rawResponse||!rawLength||rawCapacity<256)))
        return fail("CONTROL_FRAME_PARAMETER_INVALID");
    FastBee::ModbusOutputFrame frame=requested;
    if(frame.hardwareWindowMs && frame.hardwareWindowMs!=target.hardwareWindowMs)
        return fail("CONTROL_HARDWARE_WINDOW_MISMATCH");
    frame.hardwareWindowMs=target.hardwareWindowMs;
    if(!frame.valid())return fail("CONTROL_FRAME_PARAMETER_INVALID");
    FastBee::ModbusWriteTicket ticket;ticket.frame=&frame;ticket.target=target;ticket.context=context;
    Slot* reservation=nullptr;
    uint64_t previousHardwareUntil=0,hardwareStartedAt=0;
    {
        RecursiveMutexGuard lock(_mutex,0);
        uint64_t now=0;
        if(!lock.isLocked())return fail("CONTROL_BUSY");
        if(!modbusNow(now))return fail("CONTROL_CLOCK_UNCONFIRMED");
        reservation=acquireOrdinaryModbusLocked(target,context,result,now,workerGeneration);
        if(!reservation)return fail(result.reason);
        auto& slot=*reservation;
        if(slot.workerEmergencyStop)return fail("CONTROL_STOP_ENDPOINT_MISMATCH");
        ticket.gate=&slot.gate;ticket.armed=(++slot.sequence<<2U)|1U;
        ticket.acquiredMs=slot.lease.acquiredMs;ticket.holdMs=slot.lease.holdMs;
        if(context.uartOrigin.required) {
            for(const auto& entry:uartPortById) {
                const uint8_t port=entry.second;
                if(port&&port<FastBee::UartPortLease::PORT_COUNT&&uartPortLeases[port].owned()&&
                    context.uartOrigin.matches(context.configEpoch,uartReceiveGenerations.current(port))) {
                    ticket.uartGeneration=uartReceiveGenerations.handle(port);break;
                }
            }
            if(!ticket.uartGeneration){retireModbus(slot,context.request.operation);return fail("CONTROL_SOURCE_STALE");}
        }
        previousHardwareUntil=slot.hardwareUntilMs;hardwareStartedAt=now;
        if(frame.hardwareWindowMs) slot.hardwareUntilMs=UINT64_MAX;
        slot.inFlight=true;slot.gate.store(ticket.armed,std::memory_order_release);
        fillModbusResult(slot,now,result);
    }
    // Never retain the PM lock through a potentially blocking bus transaction.
    output=handler.writeOrdinaryFrameOnce(target,frame,ticket,timeout,rawResponse,rawCapacity,rawLength);
    {
        RecursiveMutexGuard lock(_mutex);
        if(!lock.isLocked()) {
            ticket.gate->fetch_or(3U,std::memory_order_acq_rel);
            result.attempted=output.outputAttempted;result.accepted=false;
            result.reason="CONTROL_TERMINAL_UNCONFIRMED";output.error=ONESHOT_CONTROL_REJECTED;
            output.controlReason=result.reason;return output;
        }
        auto& slot=*reservation;
        slot.gate.fetch_or(3U,std::memory_order_acq_rel);slot.inFlight=false;
        if(frame.hardwareWindowMs) {
            uint64_t completedAt=0;
            if(!output.outputAttempted)slot.hardwareUntilMs=previousHardwareUntil;
            else if(modbusNow(completedAt) && completedAt>=hardwareStartedAt && completedAt<=UINT64_MAX-frame.hardwareWindowMs) {
                const uint64_t due=completedAt+frame.hardwareWindowMs;
                slot.hardwareUntilMs=previousHardwareUntil>due?previousHardwareUntil:due;
            }
            // Unknown monotonic time leaves UINT64_MAX, never guessed expiry.
        }
        result.attempted=output.outputAttempted;result.accepted=output.error==ONESHOT_SUCCESS;
        result.reason=output.controlReason?output.controlReason:"CONTROL_RESULT_UNKNOWN";
        if(!result.accepted) {
            if(slot.failures!=UINT32_MAX)++slot.failures;
            if(!slot.firstFailureOperation){slot.firstFailureOperation=context.request.operation;slot.firstFailure=result.reason;}
            retireModbus(slot,context.request.operation);
        }
        if(slot.terminalRequested) {
            const bool cancelled=slot.terminalCancelled;slot.terminalRequested=false;
            finishOrdinaryModbusLocked(context,cancelled);
        }
        uint64_t now=0;
        if(!modbusNow(now)||!sourceEpochsCurrent(context)||!isUartEventOriginCurrent(context.uartOrigin)||
            OC::expired(slot.lease,now))retireModbus(slot,_nextOrdinaryControlOperation);
        fillModbusResult(slot,now,result);
    }
    return output;
}
bool PeripheralManager::reserveOrdinaryModbusWork(const FastBee::ModbusControlTarget& target,
    const Context& context, uint64_t& workGeneration, Result& result, bool emergencyStop) {
    workGeneration = 0; result = {}; result.evaluated = true;
    auto refuse = [&](const char* reason) { result.reason = reason; return false; };
    if (!target.valid() || (target.kind!=FastBee::ModbusControlTarget::Kind::Motor &&
        target.kind!=FastBee::ModbusControlTarget::Kind::Frame) ||
        (emergencyStop && target.kind!=FastBee::ModbusControlTarget::Kind::Motor))
        return refuse("CONTROL_WORK_MAPPING_UNCONFIRMED");
    RecursiveMutexGuard lock(_mutex, 0);
    uint64_t now = 0;
    if (!lock.isLocked()) return refuse("CONTROL_BUSY");
    if (!modbusNow(now)) return refuse("CONTROL_CLOCK_UNCONFIRMED");
    if (!sourceEpochsCurrent(context) || !isUartEventOriginCurrent(context.uartOrigin))
        return refuse("CONTROL_SOURCE_STALE");
    if (!context.request.operation || context.request.operation > _nextOrdinaryControlOperation ||
        context.request.operation <= _ordinaryModbusRetiredFloor || _nextOrdinaryControlGeneration == UINT64_MAX)
        return refuse("CONTROL_OPERATION_STALE");
    Slot* slot = nullptr;
    if (emergencyStop) {
        // Emergency motor stop is a distinct bounded safety request. It grants
        // no movement/parameter permit, and cannot issue UART bytes until the
        // previous worker has published its terminal result and released work.
        OC::Lease empty;
        OC::Request request = context.request; request.intent = OC::Intent::Write;
        if (OC::inspect(empty, request, now, false) != OC::Decision::Acquire)
            return refuse("CONTROL_STOP_CONTEXT_INVALID");
        for (auto& candidate : _ordinaryModbus)
            if (candidate.target.instance == target.instance && candidate.target.slave == target.slave) {
                slot = &candidate; break;
            }
        if (slot && slot->pendingStopOperation) return refuse("CONTROL_STOP_PENDING");
        if (slot && (slot->lease.phase != OC::Phase::Idle || slot->workerReserved || slot->inFlight) &&
            !(slot->lease.phase == OC::Phase::Executing && !slot->workerReserved && !slot->inFlight &&
              slot->lease.operation == context.request.operation && OC::sameActor(slot->lease.actor, context.request.actor))) {
            if (!slot->target.sameMapping(target) || context.request.operation <= slot->lease.operation ||
                context.request.operation <= slot->lease.retiredThrough)
                return refuse("CONTROL_STOP_TARGET_STALE");
            slot->pendingStopOperation = context.request.operation;
            slot->pendingStopActor = context.request.actor;
            slot->pendingStopGeneration = ++_nextOrdinaryControlGeneration;
            slot->pendingStopAcquiredMs = now;
            slot->pendingStopTerminal = false; slot->pendingStopCancelled = false;
            slot->terminalRequested = true; slot->terminalCancelled = true;
            retireModbus(*slot, slot->lease.operation);
            workGeneration = slot->pendingStopGeneration;
            fillModbusResult(*slot, now, result);
            result.accepted = true; result.reason = "CONTROL_EMERGENCY_STOP_QUEUED";
            return true;
        }
    }
    slot = acquireOrdinaryModbusLocked(target, context, result, now);
    if (!slot) return false;
    if (slot->workerReserved || slot->inFlight || _nextOrdinaryControlGeneration == UINT64_MAX)
        return refuse("CONTROL_WORKER_PENDING");
    slot->workerReserved = true; slot->workerEmergencyStop = emergencyStop;
    workGeneration = slot->workerGeneration = ++_nextOrdinaryControlGeneration;
    fillModbusResult(*slot, now, result);
    result.accepted = true; result.reason = "CONTROL_WORK_RESERVED";
    return true;
}

bool PeripheralManager::activateOrdinaryModbusWork(const FastBee::ModbusControlTarget& target,
    const Context& context, uint64_t workGeneration, const char** rejectionReason) {
    if (rejectionReason) *rejectionReason = nullptr;
    auto refuse = [&](const char* reason) { if (rejectionReason) *rejectionReason = reason; return false; };
    if (ordinaryWorkerDeadlineExpired(context)) return refuse("CONTROL_OPERATION_DEADLINE_EXCEEDED");
    RecursiveMutexGuard lock(_mutex, ordinaryWorkerLockWait(context));
    uint64_t now = 0;
    if (ordinaryWorkerDeadlineExpired(context)) return refuse("CONTROL_OPERATION_DEADLINE_EXCEEDED");
    if (!lock.isLocked()) return refuse("CONTROL_BUSY");
    if (!workGeneration) return refuse("CONTROL_WORKER_STALE");
    if (!modbusNow(now)) return refuse("CONTROL_CLOCK_UNCONFIRMED");
    if (!sourceEpochsCurrent(context) || !isUartEventOriginCurrent(context.uartOrigin))
        return refuse("CONTROL_SOURCE_STALE");
    serviceOrdinaryModbusLocked();
    for (auto& slot : _ordinaryModbus) {
        if (!slot.target.sameMapping(target)) continue;
        if (slot.pendingStopGeneration == workGeneration && slot.pendingStopOperation == context.request.operation &&
            OC::sameActor(slot.pendingStopActor, context.request.actor)) {
            if (slot.pendingStopCancelled || slot.workerReserved || slot.inFlight ||
                slot.lease.invocationActive || slot.lease.phase != OC::Phase::Idle ||
                now < slot.pendingStopAcquiredMs ||
                (context.request.holdMs && now - slot.pendingStopAcquiredMs >= context.request.holdMs))
                return refuse("CONTROL_STOP_PENDING");
            OC::Request request = context.request; request.intent = OC::Intent::Write;
            const auto decision = OC::inspect(slot.lease, request, now, false);
            if (decision != OC::Decision::Acquire) return refuse(refused(decision));
            slot.lease.actor = request.actor; slot.lease.operation = request.operation;
            slot.lease.generation = workGeneration;
            slot.lease.acquiredMs = slot.pendingStopAcquiredMs; slot.lease.holdMs = request.holdMs;
            slot.lease.phase = OC::Phase::Executing; slot.lease.invocationActive = true;
            slot.context = context;
            slot.terminalRequested = slot.pendingStopTerminal; slot.terminalCancelled = false;
            slot.pendingStopOperation = 0; slot.pendingStopGeneration = 0;
            slot.pendingStopActor = {}; slot.pendingStopTerminal = false; slot.pendingStopCancelled = false;
            slot.workerReserved = true; slot.workerEmergencyStop = true; slot.workerGeneration = workGeneration;
            return true;
        }
        if (!slot.workerReserved || slot.workerGeneration != workGeneration) return refuse("CONTROL_WORKER_STALE");
        if (slot.inFlight) return refuse("CONTROL_WORKER_PENDING");
        if (slot.lease.operation != context.request.operation) return refuse("CONTROL_OPERATION_STALE");
        if (!OC::sameActor(slot.lease.actor, context.request.actor)) return refuse("CONTROL_SOURCE_STALE");
        if (OC::expired(slot.lease, now)) return refuse("CONTROL_LEASE_EXPIRED");
        if (slot.lease.phase != OC::Phase::Executing) return refuse("CONTROL_WORK_PHASE_RETIRED");
        return true;
    }
    return refuse("CONTROL_WORK_MAPPING_UNCONFIRMED");
}

bool PeripheralManager::ordinaryModbusWorkCurrent(const FastBee::ModbusControlTarget& target,
    const Context& context, uint64_t workGeneration, const char** rejectionReason) {
    if (rejectionReason) *rejectionReason = nullptr;
    auto refuse = [&](const char* reason) { if (rejectionReason) *rejectionReason = reason; return false; };
    if (ordinaryWorkerDeadlineExpired(context)) return refuse("CONTROL_OPERATION_DEADLINE_EXCEEDED");
    RecursiveMutexGuard lock(_mutex, ordinaryWorkerLockWait(context));
    uint64_t now = 0;
    if (ordinaryWorkerDeadlineExpired(context)) return refuse("CONTROL_OPERATION_DEADLINE_EXCEEDED");
    if (!lock.isLocked()) return refuse("CONTROL_BUSY");
    if (!workGeneration) return refuse("CONTROL_WORKER_STALE");
    if (!modbusNow(now)) return refuse("CONTROL_CLOCK_UNCONFIRMED");
    if (!sourceEpochsCurrent(context) || !isUartEventOriginCurrent(context.uartOrigin))
        return refuse("CONTROL_SOURCE_STALE");
    for (auto& slot : _ordinaryModbus) {
        if (!slot.target.sameMapping(target)) continue;
        if (!slot.workerReserved || slot.workerGeneration != workGeneration) return refuse("CONTROL_WORKER_STALE");
        if (slot.lease.operation != context.request.operation) return refuse("CONTROL_OPERATION_STALE");
        if (!OC::sameActor(slot.lease.actor, context.request.actor)) return refuse("CONTROL_SOURCE_STALE");
        if (OC::expired(slot.lease, now)) return refuse("CONTROL_LEASE_EXPIRED");
        if (slot.lease.phase != OC::Phase::Executing) return refuse("CONTROL_WORK_PHASE_RETIRED");
        return true;
    }
    return refuse("CONTROL_WORK_MAPPING_UNCONFIRMED");
}

bool PeripheralManager::finishOrdinaryModbusWork(const FastBee::ModbusControlTarget& target,
    const Context& context, uint64_t workGeneration, bool failed) {
    RecursiveMutexGuard lock(_mutex);
    if (!lock.isLocked() || !workGeneration) return false;
    for (auto& slot : _ordinaryModbus) {
        if (!slot.target.sameMapping(target)) continue;
        if (slot.pendingStopGeneration == workGeneration && slot.pendingStopOperation == context.request.operation &&
            OC::sameActor(slot.pendingStopActor, context.request.actor)) {
            slot.pendingStopOperation = 0; slot.pendingStopGeneration = 0; slot.pendingStopActor = {};
            slot.pendingStopTerminal = false; slot.pendingStopCancelled = false;
            if (slot.lease.retiredThrough < context.request.operation)
                slot.lease.retiredThrough = context.request.operation;
            return true; // Cancelled before activation: zero UART attempts.
        }
        if (!slot.workerReserved || slot.workerGeneration != workGeneration || slot.inFlight ||
            slot.lease.operation != context.request.operation || !OC::sameActor(slot.lease.actor, context.request.actor))
            return false;
        slot.workerReserved = false; slot.workerGeneration = 0; slot.workerEmergencyStop = false;
        if (failed) retireModbus(slot, context.request.operation);
        if (slot.terminalRequested) {
            const bool cancelled = slot.terminalCancelled || failed;
            slot.terminalRequested = false;
            finishOrdinaryModbusLocked(context, cancelled);
        }
        return true;
    }
    return false; // Duplicate/late worker must not finalize a replacement.
}

#else
bool PeripheralManager::beginOrdinaryModbusMaintenance(uint64_t,uint32_t,FastBee::OrdinaryControlContext& context) {
    context={};return false;
}
OneShotResult PeripheralManager::submitOrdinaryModbusFrame(ModbusHandler&,
    const FastBee::ModbusControlTarget&,const FastBee::ModbusOutputFrame&,
    const FastBee::OrdinaryControlContext&,FastBee::OrdinaryControlResult& result,
    uint16_t,uint8_t*,size_t,size_t* length,uint64_t) {
    if(length) *length=0;
    result={};result.evaluated=true;result.reason="CONTROL_TARGET_UNSUPPORTED";
    OneShotResult output;output.error=ONESHOT_CONTROL_REJECTED;output.controlReason=result.reason;return output;
}
void PeripheralManager::finishOrdinaryModbusLocked(const FastBee::OrdinaryControlContext&, bool) {}
void PeripheralManager::serviceOrdinaryModbusLocked() {}
OneShotResult PeripheralManager::submitOrdinaryModbusRelay(ModbusHandler&,
    const FastBee::ModbusControlTarget&, uint8_t, uint16_t, uint16_t,
    const FastBee::OrdinaryControlContext&, FastBee::OrdinaryControlResult& result, uint16_t, bool, uint64_t) {
    result = {}; result.evaluated = true; result.reason = "CONTROL_TARGET_UNSUPPORTED";
    OneShotResult output; output.error = ONESHOT_CONTROL_REJECTED;
    output.controlReason = result.reason; return output;
}
bool PeripheralManager::reserveOrdinaryModbusWork(const FastBee::ModbusControlTarget&,
    const FastBee::OrdinaryControlContext&, uint64_t& work, FastBee::OrdinaryControlResult& result, bool) {
    work = 0; result = {}; result.evaluated = true; result.reason = "CONTROL_TARGET_UNSUPPORTED"; return false;
}
bool PeripheralManager::activateOrdinaryModbusWork(const FastBee::ModbusControlTarget&,
    const FastBee::OrdinaryControlContext&, uint64_t, const char** reason) {
    if (reason) *reason = "CONTROL_TARGET_UNSUPPORTED";
    return false;
}
bool PeripheralManager::ordinaryModbusWorkCurrent(const FastBee::ModbusControlTarget&,
    const FastBee::OrdinaryControlContext&, uint64_t, const char** reason) {
    if (reason) *reason = "CONTROL_TARGET_UNSUPPORTED";
    return false;
}
bool PeripheralManager::finishOrdinaryModbusWork(const FastBee::ModbusControlTarget&,
    const FastBee::OrdinaryControlContext&, uint64_t, bool) { return false; }

#endif
