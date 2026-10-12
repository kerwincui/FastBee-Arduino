#include "protocols/ModbusHandler.h"
#include "core/PeripheralManager.h"
#include <cstdio>

#if FASTBEE_ENABLE_MODBUS
namespace {
struct ManagedDelayBusGuard {
    SemaphoreHandle_t semaphore;
    bool held;
    explicit ManagedDelayBusGuard(SemaphoreHandle_t value, bool terminal=false) : semaphore(value),
        held(value && xSemaphoreTake(value, terminal ? portMAX_DELAY : 0) == pdTRUE) {}
    ~ManagedDelayBusGuard() { if(held)xSemaphoreGive(semaphore); }
};
bool sameDelayReceipt(const ModbusDelayReceipt& a,const ModbusDelayReceipt& b) {
    return a.instance==b.instance && a.configuration==b.configuration &&
        a.writeSequence==b.writeSequence && a.slave==b.slave &&
        a.function==b.function && a.address==b.address;
}
const char* managedDelayPhase(CoilDelayTask::Phase phase) {
    switch(phase) {
    case CoilDelayTask::Phase::Pending:return "PENDING";
    case CoilDelayTask::Phase::Acknowledged:return "ACKNOWLEDGED";
    case CoilDelayTask::Phase::ResultUnknown:return "RESULT_UNKNOWN";
    case CoilDelayTask::Phase::Retired:return "RETIRED";
    default:return "EMPTY";
    }
}
void managedDelayNumber(JsonObject value,const char* name,uint64_t number) {
    char text[21];std::snprintf(text,sizeof(text),"%llu",static_cast<unsigned long long>(number));
    value[name]=text;
}
}
bool ModbusHandler::addOrdinaryCoilDelayTask(uint8_t slave,uint16_t address,unsigned long delayMs,
    bool value,bool registerMode,const ModbusDelayReceipt& receipt,
    const FastBee::OrdinaryControlContext& context,uint64_t* operationId) {
    return scheduleOrdinaryDelay(slave,address,delayMs,value,registerMode,receipt,context,operationId,false);
}
bool ModbusHandler::retainOrdinaryHardwareDelay(uint8_t slave,unsigned long delayMs,
    const ModbusDelayReceipt& receipt,const FastBee::OrdinaryControlContext& context,uint64_t* operationId) {
    return scheduleOrdinaryDelay(slave,receipt.address,delayMs,false,receipt.function==6,
        receipt,context,operationId,true);
}
bool ModbusHandler::scheduleOrdinaryDelay(uint8_t slave,uint16_t address,unsigned long delayMs,
    bool value,bool registerMode,const ModbusDelayReceipt& receipt,
    const FastBee::OrdinaryControlContext& supplied,uint64_t* operationId,bool hardwareOnly) {
    if(operationId)*operationId=0;
    OrdinaryCoilDelayTask* selected=nullptr;
    OrdinaryCoilDelayTask prepared, previous;
    bool upgrading=false;
    {
        ManagedDelayBusGuard lock(_oneShotSemaphore);
        uint64_t now=0;
        if(!lock.held || !isInitialized || config.mode!=MODBUS_MASTER || !coilDelayTimeLocked(now) ||
            !_delayInstance || !_delayConfiguration || !delayMs || delayMs>26000 ||
            now>UINT64_MAX-delayMs || _delayNextOperation==UINT64_MAX ||
            _delayWriteSequence==UINT64_MAX || !receipt.writeSequence ||
            receipt.instance!=_delayInstance || receipt.configuration!=_delayConfiguration ||
            !slave || slave>247 || receipt.slave!=slave ||
            receipt.writeSequence!=_delayWriteSequence || !sameDelayReceipt(receipt,_delayLastWrite) ||
            !_ordinaryDelayAckOperation || _ordinaryDelayAckOperation!=supplied.request.operation ||
            !FastBee::OrdinaryControl::sameActor(_ordinaryDelayAckActor,supplied.request.actor) ||
            supplied.modbusMaintenance || _ordinaryDelayAckContext.modbusMaintenance) return false;
        // Copy the actual initial invocation, never accept altered epochs,
        // deadlines, holds or UART/rule bindings in a followup argument.
        prepared.context=_ordinaryDelayAckContext;
        if(hardwareOnly) {
            if(!_ordinaryDelayAckWindowMs || delayMs!=_ordinaryDelayAckWindowMs)return false;
            prepared.target=_ordinaryDelayAckTarget;
        } else {
            if(_ordinaryDelayAckWindowMs) {
                // Hardware + NC final is the same channel after its hardware
                // timer, with the existing 500ms settling interval.
                if(delayMs!=_ordinaryDelayAckWindowMs+500U)return false;
                bool endpoint=registerMode && receipt.function==6 && address==receipt.address;
                if(!registerMode && receipt.function==5) {
                    for(uint8_t i=0;i<config.master.deviceCount;++i) {
                        const auto& device=config.master.devices[i];
                        if(device.enabled && device.slaveAddress==slave && !std::strcmp(device.deviceType,"relay") &&
                            !device.controlProtocol && device.delayMode==0 && receipt.address>=0x0200 &&
                            uint32_t(receipt.address)<0x0200U+device.channelCount &&
                            uint32_t(device.coilBase)+receipt.address-0x0200U==address)endpoint=true;
                    }
                }
                if(!endpoint)return false;
            } else if(receipt.address!=address || receipt.function!=(registerMode?6:5))return false;
            FastBee::ModbusOutputFrame finalFrame;
            if(!finalFrame.single(slave,registerMode?6:5,address,value?(registerMode?1:0xff00):0) ||
                !ordinaryFrameTargetLocked(finalFrame,prepared.target) || prepared.target.hardwareWindowMs)return false;
            if(prepared.context.request.holdMs && prepared.context.request.holdMs<=delayMs)return false;
        }
        for(auto& task:ordinaryCoilDelayTasks) {
            if(task.phase==CoilDelayTask::Phase::Pending && task.hardwareOnly && !hardwareOnly &&
                !task.running && !task.workGeneration && sameDelayReceipt(task.receipt,receipt) &&
                task.context.request.operation==supplied.request.operation &&
                FastBee::OrdinaryControl::sameActor(task.context.request.actor,supplied.request.actor)) {
                selected=&task;upgrading=true;previous=task;break;
            }
        }
        if(!upgrading) {
            if(receipt.writeSequence<=_delayArmedSequence)return false;
            for(auto& task:ordinaryCoilDelayTasks)
                if(!task.running && !task.workGeneration && task.phase!=CoilDelayTask::Phase::Pending &&
                    task.phase!=CoilDelayTask::Phase::ResultUnknown) {selected=&task;break;}
        }
        if(!selected)return false;
        prepared.receipt=receipt;prepared.operation=upgrading?previous.operation:++_delayNextOperation;
        prepared.dueMs=now+delayMs;prepared.coilValue=value;prepared.hardwareOnly=hardwareOnly;
        prepared.hardwareWindowMs=_ordinaryDelayAckWindowMs;
        prepared.phase=CoilDelayTask::Phase::Pending;prepared.error=ONESHOT_SUCCESS;
        prepared.reason=hardwareOnly?"HARDWARE_WINDOW_OBSERVING":"CONTROL_WORK_ADMISSION_PENDING";
        prepared.running=!hardwareOnly;
        *selected=prepared;
        _delayArmedSequence=receipt.writeSequence;
        if(hardwareOnly) {
            if(operationId)*operationId=prepared.operation;
            return true; // PM already retained attempted hardware timing effect.
        }
    }
    // No bus -> PM lock inversion, including rejected/cancelled admission.
    auto& pm=PeripheralManager::getInstance();
    FastBee::OrdinaryControlResult reserved;
    uint64_t work=0;
    const bool admitted=pm.reserveOrdinaryModbusWork(prepared.target,prepared.context,work,reserved);
    bool installed=false;
    {
        ManagedDelayBusGuard lock(_oneShotSemaphore,true);
        if(lock.held && selected->operation==prepared.operation && selected->running) {
            uint64_t now=0;
            if(admitted && isInitialized && config.mode==MODBUS_MASTER && coilDelayTimeLocked(now) &&
                receipt.configuration==_delayConfiguration && receipt.instance==_delayInstance &&
                sameDelayReceipt(receipt,_delayLastWrite) && selected->phase==CoilDelayTask::Phase::Pending) {
                selected->workGeneration=work;selected->reason="SCHEDULED";
                selected->running=false;installed=true;
            } else {
                // A hardware observation survives failed software admission.
                // Its actual physical barrier remains separately owned by PM.
                if(upgrading)*selected=previous;
                else {
                    selected->phase=CoilDelayTask::Phase::Retired;
                    selected->error=ONESHOT_CONTROL_REJECTED;
                    selected->reason=admitted?"CONTROL_DELAY_ADMISSION_CHANGED":reserved.reason;
                    selected->running=false;
                }
            }
        }
    }
    if(admitted && !installed) {
        const bool released=pm.finishOrdinaryModbusWork(prepared.target,prepared.context,work,true);
        if(!released) {
            ManagedDelayBusGuard lock(_oneShotSemaphore,true);
            if(lock.held && selected->operation==prepared.operation) {
                selected->target=prepared.target;selected->context=prepared.context;
                selected->workGeneration=work;selected->phase=CoilDelayTask::Phase::Retired;
                selected->reason="CONTROL_DELAY_CLEANUP_PENDING";selected->running=false;
            }
        }
    }
    if(installed && operationId)*operationId=prepared.operation;
    return installed;
}
void ModbusHandler::retireOrdinaryDelayWork() {
    for(auto& record:ordinaryCoilDelayTasks) {
        OrdinaryCoilDelayTask copy;
        {
            ManagedDelayBusGuard lock(_oneShotSemaphore,true);
            if(!lock.held)return;
            if(record.running || !record.workGeneration || record.phase==CoilDelayTask::Phase::Pending)continue;
            record.running=true;copy=record;
        }
        const bool finished=PeripheralManager::getInstance().finishOrdinaryModbusWork(
            copy.target,copy.context,copy.workGeneration,copy.phase!=CoilDelayTask::Phase::Acknowledged);
        {
            ManagedDelayBusGuard lock(_oneShotSemaphore,true);
            if(!lock.held)return;
            if(record.operation==copy.operation && record.workGeneration==copy.workGeneration) {
                if(finished)record.workGeneration=0;
                record.running=false;
            }
        }
    }
}
void ModbusHandler::processOrdinaryCoilDelayTasks() {
    retireOrdinaryDelayWork();
    for(auto& record:ordinaryCoilDelayTasks) {
        OrdinaryCoilDelayTask copy;
        bool due=false;
        {
            ManagedDelayBusGuard lock(_oneShotSemaphore);
            if(!lock.held)return;
            if(record.running || record.phase!=CoilDelayTask::Phase::Pending)continue;
            uint64_t now=0;
            if(!coilDelayTimeLocked(now))return;
            if(record.hardwareOnly) {
                if(now>=record.dueMs) {
                    record.phase=CoilDelayTask::Phase::Retired;
                    record.reason="HARDWARE_WINDOW_ELAPSED_PHYSICAL_UNVERIFIED";
                }
                continue;
            }
            if(!isInitialized || config.mode!=MODBUS_MASTER || !record.workGeneration ||
                record.receipt.instance!=_delayInstance || record.receipt.configuration!=_delayConfiguration) {
                record.phase=CoilDelayTask::Phase::Retired;record.reason="HANDLER_GENERATION_CHANGED";
                continue;
            }
            due=now>=record.dueMs;record.running=true;copy=record;
        }
        auto& pm=PeripheralManager::getInstance();
        const bool current=pm.ordinaryModbusWorkCurrent(copy.target,copy.context,copy.workGeneration);
        if(current && !due) {
            ManagedDelayBusGuard lock(_oneShotSemaphore,true);
            if(lock.held && record.operation==copy.operation)record.running=false;
            continue;
        }
        OneShotResult output;output.error=ONESHOT_CONTROL_REJECTED;
        output.controlReason="CONTROL_DELAY_SOURCE_RETIRED";
        if(current) {
            FastBee::ModbusOutputFrame frame;
            frame.single(copy.target.slave,copy.target.function,copy.target.first,
                copy.coilValue?(copy.target.function==5?0xff00:1):0);
            FastBee::OrdinaryControlResult result;
            output=pm.submitOrdinaryModbusFrame(*this,copy.target,frame,copy.context,result,
                0,nullptr,0,nullptr,copy.workGeneration);
        }
        {
            // Publish the real terminal result before releasing producer work.
            // No PM lock is held. running prevents duplicate handle invocations.
            ManagedDelayBusGuard lock(_oneShotSemaphore,true);
            if(!lock.held)return; // Fail closed; do not release unrecorded work.
            if(record.operation!=copy.operation || record.workGeneration!=copy.workGeneration)return;
            record.attempted=output.outputAttempted;record.error=output.error;
            record.exceptionCode=output.exceptionCode;record.running=false;
            if(output.error==ONESHOT_SUCCESS && output.delayReceipt.writeSequence) {
                record.phase=CoilDelayTask::Phase::Acknowledged;
                record.reason="FINAL_COMMAND_ACKNOWLEDGED";
            } else {
                if(record.error==ONESHOT_SUCCESS)record.error=ONESHOT_CRC_ERROR;
                record.phase=output.outputAttempted?CoilDelayTask::Phase::ResultUnknown:CoilDelayTask::Phase::Retired;
                record.reason=output.controlReason?output.controlReason:"FINAL_RESULT_UNKNOWN";
                if(_delayFailures!=UINT32_MAX)++_delayFailures;
                if(!_delayFirstFailureOperation) {
                    _delayFirstFailureOperation=record.operation;_delayFirstFailure=record.error;
                }
            }
        }
        retireOrdinaryDelayWork();
        return; // At most one actual final transaction per handle tick.
    }
    retireOrdinaryDelayWork();
}
void ModbusHandler::appendOrdinaryDelayStatusLocked(JsonArray records) {
    for(const auto& task:ordinaryCoilDelayTasks) {
        if(task.phase==CoilDelayTask::Phase::Empty)continue;
        JsonObject record=records.add<JsonObject>();
        record["handlerInstance"]=task.receipt.instance;record["configuration"]=task.receipt.configuration;
        managedDelayNumber(record,"operationId",task.operation);
        managedDelayNumber(record,"sourceOperation",task.context.request.operation);
        managedDelayNumber(record,"sourceActor",task.context.request.actor.identity);
        managedDelayNumber(record,"initialWriteSequence",task.receipt.writeSequence);
        record["source"]=static_cast<uint8_t>(task.context.request.actor.source);
        record["slaveAddress"]=task.receipt.slave;record["function"]=task.target.function;
        record["address"]=task.target.first;record["targetValue"]=task.coilValue?1:0;
        record["phase"]=managedDelayPhase(task.phase);record["reason"]=task.reason;
        record["hardwareOnly"]=task.hardwareOnly;record["hardwareWindowMs"]=task.hardwareWindowMs;
        record["pending"]=task.phase==CoilDelayTask::Phase::Pending;
        record["running"]=task.running;
        record["cleanupPending"]=task.workGeneration && task.phase!=CoilDelayTask::Phase::Pending;
        record["initialCommandAcknowledged"]=true;
        record["finalCommandAcknowledged"]=!task.hardwareOnly && task.phase==CoilDelayTask::Phase::Acknowledged;
        record["attempted"]=task.attempted;record["error"]=static_cast<uint8_t>(task.error);
        record["exceptionCode"]=task.exceptionCode;record["physicalOutputVerified"]=false;
    }
}
#endif
