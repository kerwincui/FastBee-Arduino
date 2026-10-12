#include "protocols/ModbusHandler.h"
#include <esp_timer.h>
#include "protocols/ModbusMotorReference.h"
#if FASTBEE_ENABLE_MODBUS
namespace {
struct OrdinaryBusGuard {
    SemaphoreHandle_t lock;
    bool held;
    explicit OrdinaryBusGuard(SemaphoreHandle_t value) : lock(value),
        held(value && xSemaphoreTake(value, pdMS_TO_TICKS(2000)) == pdTRUE) {}
    ~OrdinaryBusGuard() { if (held) xSemaphoreGive(lock); }
};
}
namespace {
bool ordinaryAddressInRange(uint16_t address, uint16_t first, uint8_t count) {
    return count && address >= first && uint32_t(address) < uint32_t(first) + count;
}
bool ordinaryRelayMappingValid(const ModbusSubDevice& device) {
    if (device.controlProtocol > 1 || !device.channelCount ||
        uint32_t(device.coilBase) + device.channelCount > 65536U) return false;
    if (device.batchRegister && (device.batchRegType > 1 ||
        (!device.batchRegType && device.channelCount > 16) ||
        (device.controlProtocol && ordinaryAddressInRange(device.batchRegister,
            device.coilBase, device.channelCount)))) return false;
    // The saved vendor timer uses FC05 at 0x0200+channel. A normal coil
    // alias there must not bypass the hardware-effect reservation via M1.
    return device.controlProtocol || device.delayMode != 0 ||
        uint32_t(device.coilBase) + device.channelCount <= 0x0200U ||
        uint32_t(device.coilBase) >= 0x0200U + device.channelCount;
}
bool ordinaryOwnsAddress(const ModbusSubDevice& device, bool coil, uint16_t address) {
    if (!device.enabled) return false;
    // Ownership is independent of the requested value, configured resolution,
    // or FC06/FC16 preference. Invalid profiles cannot hide overlapping words.
    if (!std::strcmp(device.deviceType, "relay")) {
        if (coil) return (device.controlProtocol != 1 &&
                ordinaryAddressInRange(address, device.coilBase, device.channelCount)) ||
            (!device.controlProtocol && device.delayMode == 0 &&
                ordinaryAddressInRange(address, 0x0200, device.channelCount));
        return (device.controlProtocol && ordinaryAddressInRange(address,
                device.coilBase, device.channelCount)) ||
            (device.batchRegister && address == device.batchRegister);
    }
    if (coil) return false;
    if (!std::strcmp(device.deviceType, "pwm"))
        return ordinaryAddressInRange(address, device.pwmRegBase, device.channelCount);
    if (!std::strcmp(device.deviceType, "pid")) {
        for (uint16_t reg : device.pidAddrs) if (reg == address) return true;
    } else if (!std::strcmp(device.deviceType, "motor")) {
        for (uint16_t reg : device.motorRegs) if (reg == address) return true;
    }
    return false;
}
bool ordinaryUniqueAddress(const ModbusSubDevice* devices, uint8_t count, uint8_t slave,
    bool coil, uint16_t address, uint8_t& owner) {
    owner = 0xff;
    for (uint8_t i = 0; i < count; ++i) {
        if (devices[i].slaveAddress != slave || !ordinaryOwnsAddress(devices[i], coil, address)) continue;
        if (owner != 0xff) return false;
        owner = i;
    }
    return owner != 0xff;
}
bool frameCoversWord(const ModbusSubDevice& device, uint8_t fc, uint16_t address, uint16_t value) {
    if (!device.enabled) return false;
    const bool coil = fc == 5 || fc == 15;
    if (!std::strcmp(device.deviceType,"relay")) {
        if (!ordinaryRelayMappingValid(device)) return false;
        if (coil) return !device.controlProtocol && address >= device.coilBase &&
            uint32_t(address) < uint32_t(device.coilBase) + device.channelCount;
        if (device.controlProtocol && address >= device.coilBase &&
            uint32_t(address) < uint32_t(device.coilBase) + device.channelCount)
            return value <= 1 || device.delayMode == 2;
        if (device.batchRegister && address == device.batchRegister) {
            if (device.batchRegType == 1) return value <= 1;
            if (device.batchRegType || device.channelCount > 16) return false;
            return value <= (device.channelCount == 16 ? 0xffffU : (1U << device.channelCount)-1U);
        }
        return false;
    }
    if (coil) return false;
    if (!std::strcmp(device.deviceType,"pwm"))
        return device.channelCount && uint32_t(device.pwmRegBase) + device.channelCount <= 65536U &&
            address >= device.pwmRegBase && uint32_t(address) < uint32_t(device.pwmRegBase) + device.channelCount &&
            device.pwmResolution && device.pwmResolution <= 16 &&
            value <= (device.pwmResolution == 16 ? 0xffffU : (1U << device.pwmResolution)-1U);
    if (!std::strcmp(device.deviceType,"pid")) {
        if (device.pidAddrs[0] == address) return false;
        unsigned matches = 0;
        for (unsigned i=1;i<6;++i) if (device.pidAddrs[i] == address) ++matches;
        return matches == 1; // PV is read-only; repeated unspecified addresses are ambiguous.
    }
    if (!std::strcmp(device.deviceType,"motor")) {
        if (!device.motorProtocol.valid || device.motorProtocol.writeFunction != fc) return false;
        // The actual transport still enforces saved action values, preparation,
        // travel bounds and once-only motor output for low-level register calls.
        for (uint16_t reg : device.motorRegs) if (reg == address) return true;
    }
    return false;
}
}
bool ModbusHandler::ordinaryFrameTargetLocked(const FastBee::ModbusOutputFrame& frame,
    FastBee::ModbusControlTarget& target) const {
    target = {};
    if (!isInitialized || config.mode != MODBUS_MASTER || !frame.valid()) return false;
    using Access = FastBee::ModbusFrameAccess;
    using Transform = FastBee::ModbusFrameTransform;
    const uint8_t slave = frame.bytes[0], fc = frame.bytes[1];
    const uint16_t address = frame.address(), count = frame.quantity();
    if (frame.access == Access::Maintenance) {
        bool matched = false;
        for (uint8_t i=0;i<config.master.deviceCount;++i) {
            const auto& device = config.master.devices[i];
            if (!device.enabled) continue;
            if (!slave) matched |= device.addressReg == address;
            else if (device.slaveAddress == slave) {
                if (fc == 0xb0) matched |= device.baudRateMode == 0;
                else matched |= device.baudRateMode == 1 &&
                    address == (device.baudRateReg ? device.baudRateReg : 0x0033);
            }
        }
        if (!matched) return false;
    } else if (frame.access == Access::RelayVendor ||
               frame.transform == Transform::RelayBatchToggle) {
        unsigned matches = 0;
        for (uint8_t i=0;i<config.master.deviceCount;++i) {
            const auto& device = config.master.devices[i];
            if (!device.enabled || device.slaveAddress != slave || std::strcmp(device.deviceType,"relay") ||
                !ordinaryRelayMappingValid(device)) continue;
            if (frame.transform == Transform::RelayBatchToggle) {
                if (device.batchRegister == address && device.batchRegister && !device.batchRegType &&
                    device.channelCount <= 16 &&
                    frame.word() == (device.channelCount == 16 ? 0xffffU : (1U << device.channelCount)-1U)) ++matches;
            } else if (!device.controlProtocol) {
                const bool singleToggle = frame.word() == 0x5500 && address >= device.coilBase &&
                    uint32_t(address) < uint32_t(device.coilBase)+device.channelCount;
                const bool batchToggle = frame.word() == 0x5a00 && address == device.coilBase;
                const bool timed = device.delayMode == 0 && address >= 0x0200 &&
                    uint32_t(address) < 0x0200U + device.channelCount;
                if (singleToggle || batchToggle || timed) ++matches;
            }
        }
        if (matches != 1) return false;
        uint8_t owner = 0xff;
        if (!ordinaryUniqueAddress(config.master.devices, config.master.deviceCount, slave,
            fc == 5 || fc == 15, address, owner)) return false;
        // A vendor frame may affect the entire relay. Do not guess semantics on
        // a mixed motor/PWM/PID slave without an explicit vendor device profile.
        if (frame.access == Access::RelayVendor)
            for (uint8_t i=0;i<config.master.deviceCount;++i)
                if (config.master.devices[i].enabled && config.master.devices[i].slaveAddress == slave &&
                    std::strcmp(config.master.devices[i].deviceType,"relay")) return false;
    } else {
        for (uint32_t offset=0;offset<count;++offset) {
            const uint16_t at = static_cast<uint16_t>(uint32_t(address)+offset);
            const uint16_t value = fc == 15 ? ((frame.bytes[7+offset/8] >> (offset%8)) & 1U) :
                fc == 16 ? static_cast<uint16_t>((frame.bytes[7+2*offset]<<8)|frame.bytes[8+2*offset]) : frame.word();
            uint8_t owner = 0xff;
            if (!ordinaryUniqueAddress(config.master.devices, config.master.deviceCount, slave,
                fc == 5 || fc == 15, at, owner) ||
                !frameCoversWord(config.master.devices[owner], fc, at, value)) return false;
        }
    }
    uint32_t hardwareWindow=0;
    // Low-level/raw aliases cannot omit the physical effect window. Derive it
    // from each uniquely mapped saved relay register, including FC10 batches.
    if(frame.access!=Access::Maintenance && frame.transform==Transform::None) {
        for(uint32_t offset=0;offset<count;++offset) {
            const uint16_t at=static_cast<uint16_t>(uint32_t(address)+offset);
            const uint16_t value=fc==16 ? static_cast<uint16_t>((frame.bytes[7+2*offset]<<8)|frame.bytes[8+2*offset]) : frame.word();
            for(uint8_t i=0;i<config.master.deviceCount;++i) {
                const auto& device=config.master.devices[i];
                if(!device.enabled || device.slaveAddress!=slave || std::strcmp(device.deviceType,"relay"))continue;
                uint32_t window=0;
                if(fc==5 && !device.controlProtocol && device.delayMode==0 &&
                    at>=0x0200 && uint32_t(at)<0x0200U+device.channelCount)
                    window=uint32_t(value>>8)*100U;
                else if((fc==6 || fc==16) && device.controlProtocol && device.delayMode==2 &&
                    at>=device.coilBase && uint32_t(at)<uint32_t(device.coilBase)+device.channelCount && value>1)
                    window=uint32_t(value-1U)*10U;
                if(window>26000)return false;
                if(window>hardwareWindow)hardwareWindow=window;
            }
        }
    }
    if(frame.hardwareWindowMs && frame.hardwareWindowMs!=hardwareWindow)return false;
    target.kind = frame.access == Access::Maintenance ?
        FastBee::ModbusControlTarget::Kind::Maintenance : FastBee::ModbusControlTarget::Kind::Frame;
    target.instance=_delayInstance; target.configuration=_delayConfiguration;
    target.slave=frame.access == Access::Maintenance ? 0 : slave; target.index=0xff;
    target.function=fc; target.first=address; target.frameQuantity=count;
    target.frameSlave=slave; target.frameAccess=frame.access; target.frameTransform=frame.transform;
    target.hardwareWindowMs=hardwareWindow;
    return target.valid();
}
bool ModbusHandler::snapshotOrdinaryFrame(const FastBee::ModbusOutputFrame& frame,
    FastBee::ModbusControlTarget& target) {
    OrdinaryBusGuard lock(_oneShotSemaphore); target={};
    return lock.held && ordinaryFrameTargetLocked(frame,target);
}
OneShotResult ModbusHandler::writeOrdinaryFrameOnce(const FastBee::ModbusControlTarget& target,
    const FastBee::ModbusOutputFrame& frame, const FastBee::ModbusWriteTicket& ticket,
    uint16_t timeout, uint8_t* rawResponse, size_t rawCapacity, size_t* rawLength) {
    OneShotResult result; result.error=ONESHOT_CONTROL_REJECTED;
    result.controlReason="CONTROL_FRAME_MAPPING_UNCONFIRMED";
    if (rawLength) *rawLength=0;
    if (!frame.valid() || !ticket.frame || !ticket.frame->same(frame) ||
        !target.sameMapping(ticket.target) ||
        ((rawResponse || rawLength || rawCapacity) && (!rawResponse || !rawLength || rawCapacity < 256))) return result;
    OrdinaryBusGuard lock(_oneShotSemaphore);
    if (!lock.held) { result.error=ONESHOT_BUSY;result.controlReason="CONTROL_BUS_BUSY";return result; }
    FastBee::ModbusControlTarget current;
    if (!ordinaryFrameTargetLocked(frame,current) || !current.sameMapping(target) ||
        !modbusSerial || _ordinaryWriteTicket) return result;
    struct ResetFrameState {
        volatile bool& reading; const FastBee::ModbusWriteTicket*& ticket;
        ~ResetFrameState(){reading=false;ticket=nullptr;}
    } reset{isOneShotReading,_ordinaryWriteTicket};
    FastBee::ModbusOutputFrame actual=frame;
    FastBee::ModbusWriteTicket actualTicket=ticket; actualTicket.frame=&actual;
    if (frame.transform == FastBee::ModbusFrameTransform::RelayBatchToggle) {
        const int64_t us=esp_timer_get_time();
        if (us < 0 || !ticket.sourceCurrent(static_cast<uint64_t>(us)/1000ULL,static_cast<uint32_t>(millis())) ||
            !ticket.gate || ticket.gate->load(std::memory_order_acquire)!=ticket.armed) return result;
        uint8_t read[8]={frame.bytes[0],3,frame.bytes[2],frame.bytes[3],0,1,0,0};
        const uint16_t crc=calculateCRC(read,6);read[6]=static_cast<uint8_t>(crc);read[7]=static_cast<uint8_t>(crc>>8);
        isOneShotReading=true;
        try { result=_executeModbusTransaction(read,sizeof(read),read[0],timeout,0,true); }
        catch (...) {result.error=ONESHOT_BUSY;result.controlReason="CONTROL_READ_ALLOCATION_FAILED";return result;}
        isOneShotReading=false;
        if(result.error!=ONESHOT_SUCCESS || result.count!=7 || result.data[1]!=3 || result.data[2]!=2) {
            if(result.error==ONESHOT_SUCCESS)result.error=ONESHOT_CRC_ERROR;
            result.controlReason="CONTROL_TOGGLE_READ_UNCONFIRMED";return result;
        }
        const uint16_t previous=static_cast<uint16_t>((result.data[3]<<8)|result.data[4]);
        if(previous & static_cast<uint16_t>(~frame.word())) {
            result.error=ONESHOT_CONTROL_REJECTED;result.controlReason="CONTROL_TOGGLE_READ_UNCONFIRMED";return result;
        }
        const uint16_t value=static_cast<uint16_t>(previous^frame.word());
        actual.bytes[4]=static_cast<uint8_t>(value>>8);actual.bytes[5]=static_cast<uint8_t>(value);
    }
    uint8_t request[256]{}, response[256]{};
    std::memcpy(request,actual.bytes,actual.length);
    const uint16_t crc=calculateCRC(request,actual.length);
    request[actual.length]=static_cast<uint8_t>(crc);request[actual.length+1]=static_cast<uint8_t>(crc>>8);
    size_t received=0; const uint64_t beforeWrite=_delayWriteSequence;
    _ordinaryWriteAttempted=false;_ordinaryWriteTicket=&actualTicket;isOneShotReading=true;
    try {
        result.error=_executeModbusTransactionRaw(request,actual.length+2,actual.bytes[0],response,
            sizeof(response),received,result.exceptionCode,timeout,0,true);
    } catch (...) {result.error=ONESHOT_BUSY;}
    isOneShotReading=false;_ordinaryWriteTicket=nullptr;
    result.outputAttempted=_ordinaryWriteAttempted;
    result.travelError=result.error==ONESHOT_TRAVEL_BLOCKED?_lastTravelError:nullptr;
    if(result.error==ONESHOT_SUCCESS) {
        bool exact=received==8;
        // Broadcast address maintenance accepts the responding slave byte;
        // this is transport evidence, not proof of which physical board changed.
        for(size_t i=actual.bytes[0]?0:1;exact&&i<6;++i)exact=response[i]==request[i];
        if(!exact)result.error=ONESHOT_CRC_ERROR;
        else {
            result.count=8;for(size_t i=0;i<8;++i)result.data[i]=response[i];
            if(actual.length==6&&(actual.bytes[1]==5||actual.bytes[1]==6)&&
                _delayWriteSequence!=beforeWrite&&_delayLastWrite.writeSequence==_delayWriteSequence&&
                _delayLastWrite.instance==_delayInstance&&_delayLastWrite.configuration==_delayConfiguration&&
                _delayLastWrite.slave==actual.bytes[0]&&_delayLastWrite.function==actual.bytes[1]&&
                _delayLastWrite.address==actual.address())result.delayReceipt=_delayLastWrite;
        }
    }
    if(result.delayReceipt.writeSequence) {
        _ordinaryDelayAckTarget=target;
        _ordinaryDelayAckContext=ticket.context;
        _ordinaryDelayAckOperation=ticket.context.request.operation;
        _ordinaryDelayAckActor=ticket.context.request.actor;
        _ordinaryDelayAckWindowMs=frame.hardwareWindowMs;
    }
    if(rawResponse && (result.error==ONESHOT_SUCCESS || result.error==ONESHOT_EXCEPTION) && received<=rawCapacity) {
        std::memcpy(rawResponse,response,received);*rawLength=received;
    }
    result.controlReason=result.error==ONESHOT_SUCCESS?"CONTROL_ACKNOWLEDGED_TRANSPORT_ONLY":
        result.outputAttempted?"CONTROL_RESULT_UNKNOWN":"CONTROL_NOT_ATTEMPTED";
    return result;
}

bool ModbusHandler::ordinaryRelayTargetLocked(uint8_t index, FastBee::ModbusControlTarget& target) const {
    target = {};
    if (!isInitialized || config.mode != MODBUS_MASTER || index >= config.master.deviceCount) return false;
    const auto& device = config.master.devices[index];
    if (!device.enabled || std::strcmp(device.deviceType, "relay") || !ordinaryRelayMappingValid(device)) return false;
    uint8_t owner = 0xff;
    for (uint16_t offset = 0; offset < device.channelCount; ++offset)
        if (!ordinaryUniqueAddress(config.master.devices, config.master.deviceCount, device.slaveAddress,
            !device.controlProtocol, static_cast<uint16_t>(device.coilBase + offset), owner) || owner != index) return false;
    if (device.batchRegister && (!ordinaryUniqueAddress(config.master.devices, config.master.deviceCount,
        device.slaveAddress, false, device.batchRegister, owner) || owner != index)) return false;
    target.instance = _delayInstance; target.configuration = _delayConfiguration;
    target.slave = device.slaveAddress; target.index = index;
    target.function = device.controlProtocol ? 6 : 5;
    target.first = device.coilBase; target.count = device.channelCount;
    target.batch = device.batchRegister; target.batchType = device.batchRegType; target.nc = device.ncMode;
    return target.valid();
}

bool ModbusHandler::ordinaryMotorTargetLocked(uint8_t index, FastBee::ModbusControlTarget& target) const {
    target = {};
    if (!isInitialized || config.mode != MODBUS_MASTER || index >= config.master.deviceCount) return false;
    const auto& device = config.master.devices[index];
    if (!device.enabled || std::strcmp(device.deviceType, "motor") || !device.motorProtocol.valid) return false;
    uint8_t owner = 0xff;
    for (uint16_t reg : device.motorRegs)
        if (!ordinaryUniqueAddress(config.master.devices, config.master.deviceCount, device.slaveAddress,
            false, reg, owner) || owner != index) return false;
    target.kind = FastBee::ModbusControlTarget::Kind::Motor;
    target.instance = _delayInstance; target.configuration = _delayConfiguration;
    target.slave = device.slaveAddress; target.index = index;
    target.function = device.motorProtocol.writeFunction;
    target.stopValue = device.motorProtocol.stopValue;
    std::memcpy(target.motorRegisters, device.motorRegs, sizeof(target.motorRegisters));
    return target.valid();
}

bool ModbusHandler::snapshotOrdinaryMotorReference(const String& ref, ModbusSubDevice& device,
    FastBee::ModbusControlTarget& target) {
    OrdinaryBusGuard lock(_oneShotSemaphore);
    target = {}; bool found = false;
    if (!lock.held || !isInitialized || config.mode != MODBUS_MASTER || ref.isEmpty()) return false;
    for (uint8_t i = 0; i < config.master.deviceCount; ++i) {
        FastBee::ModbusControlTarget candidate;
        if (!ordinaryMotorTargetLocked(i, candidate) || FastBee::motorControlReference(config.master.devices[i]) != ref) continue;
        if (found) { target = {}; return false; }
        found = true; target = candidate; device = config.master.devices[i];
    }
    return found;
}
bool ModbusHandler::snapshotOrdinaryRelay(uint8_t index, ModbusSubDevice& device,
    FastBee::ModbusControlTarget& target) {
    OrdinaryBusGuard lock(_oneShotSemaphore);
    if (!lock.held || !ordinaryRelayTargetLocked(index, target)) return false;
    device = config.master.devices[index]; return true;
}
bool ModbusHandler::snapshotControlDevice(uint8_t index, ModbusSubDevice& device,
    FastBee::ModbusControlTarget& target) {
    OrdinaryBusGuard lock(_oneShotSemaphore);
    target = {};
    if (!lock.held || !isInitialized || config.mode != MODBUS_MASTER || index >= config.master.deviceCount) return false;
    device = config.master.devices[index];
    if (!ordinaryRelayTargetLocked(index, target)) ordinaryMotorTargetLocked(index, target);
    if (!target.valid() && device.enabled) {
        target={};target.kind=FastBee::ModbusControlTarget::Kind::Snapshot;
        target.instance=_delayInstance;target.configuration=_delayConfiguration;
        target.slave=device.slaveAddress;target.index=index;
    }
    return true;
}
bool ModbusHandler::snapshotOrdinaryRelayEndpoint(uint8_t slave, uint8_t function, uint16_t address,
    ModbusSubDevice& device, FastBee::ModbusControlTarget& target) {
    OrdinaryBusGuard lock(_oneShotSemaphore);
    target = {};
    if (!lock.held) return false;
    bool found = false;
    for (uint8_t i = 0; i < config.master.deviceCount; ++i) {
        FastBee::ModbusControlTarget candidate;
        if (!ordinaryRelayTargetLocked(i, candidate) || candidate.slave != slave ||
            !candidate.accepts(function, address, 0)) continue;
        if (found) { target = {}; return false; } // Ambiguous public address is not guessed.
        found = true; target = candidate; device = config.master.devices[i];
    }
    return found;
}
bool ModbusHandler::snapshotControlSensor(const String& sensorId, SensorIdMatch& match,
    ModbusSubDevice& device, FastBee::ModbusControlTarget& target) {
    OrdinaryBusGuard lock(_oneShotSemaphore);
    target = {}; match = SensorIdMatch{};
    if (!lock.held || !isInitialized || config.mode != MODBUS_MASTER) return false;
    if (!findBySensorIdEx(sensorId, match)) return true; // Confirmed unmatched is distinct from busy.
    if (match.deviceIndex >= config.master.deviceCount) return false;
    device = config.master.devices[match.deviceIndex];
    if (!ordinaryRelayTargetLocked(match.deviceIndex, target)) ordinaryMotorTargetLocked(match.deviceIndex, target);
    if (!target.valid() && device.enabled) {
        target={};target.kind=FastBee::ModbusControlTarget::Kind::Snapshot;
        target.instance=_delayInstance;target.configuration=_delayConfiguration;
        target.slave=device.slaveAddress;target.index=match.deviceIndex;
    }
    return true;
}
OneShotResult ModbusHandler::writeOrdinaryRelayOnce(const FastBee::ModbusControlTarget& target,
    uint8_t function, uint16_t address, uint16_t value,
    const FastBee::ModbusWriteTicket& ticket, uint16_t timeout, bool toggle) {
    OneShotResult result;
    result.error = ONESHOT_CONTROL_REJECTED;
    result.controlReason = "CONTROL_TARGET_STALE";
    if (!target.sameMapping(ticket.target) || !target.accepts(function, address, value) ||
        ticket.function != function || ticket.address != address || ticket.value != value ||
        ticket.toggle != toggle || (toggle && (target.kind != FastBee::ModbusControlTarget::Kind::Relay || function != target.function || address < target.first ||
            uint32_t(address) >= uint32_t(target.first) + target.count))) return result;
    OrdinaryBusGuard lock(_oneShotSemaphore);
    if (!lock.held) { result.error = ONESHOT_BUSY; result.controlReason = "CONTROL_BUS_BUSY"; return result; }
    FastBee::ModbusControlTarget current;
    const bool mapped = target.kind == FastBee::ModbusControlTarget::Kind::Motor ?
        ordinaryMotorTargetLocked(target.index, current) : ordinaryRelayTargetLocked(target.index, current);
    if (!mapped || !target.sameMapping(current) ||
        !modbusSerial || _ordinaryWriteTicket) return result;
    struct ResetTransactionState {
        volatile bool& reading;
        const FastBee::ModbusWriteTicket*& ticket;
        ~ResetTransactionState() { reading = false; ticket = nullptr; }
    } reset{isOneShotReading, _ordinaryWriteTicket};
    if (toggle) {
        const int64_t us = esp_timer_get_time();
        if (us < 0 || !ticket.sourceCurrent(static_cast<uint64_t>(us) / 1000ULL, static_cast<uint32_t>(millis())) ||
            !ticket.gate || ticket.gate->load(std::memory_order_acquire) != ticket.armed) return result;
        uint8_t read[8] = {target.slave, static_cast<uint8_t>(function == 5 ? 1 : 3),
            static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address), 0, 1, 0, 0};
        const uint16_t crc = calculateCRC(read, 6);
        read[6] = static_cast<uint8_t>(crc); read[7] = static_cast<uint8_t>(crc >> 8);
        isOneShotReading = true;
        try { result = _executeModbusTransaction(read, sizeof(read), target.slave, timeout, 0, true); }
        catch (...) { result.error = ONESHOT_BUSY; result.controlReason = "CONTROL_READ_ALLOCATION_FAILED"; return result; }
        isOneShotReading = false;
        const bool coil = function == 5;
        if (result.error != ONESHOT_SUCCESS || result.count != (coil ? 6 : 7) ||
            result.data[1] != read[1] || result.data[2] != (coil ? 1 : 2)) {
            if (result.error == ONESHOT_SUCCESS) result.error = ONESHOT_CRC_ERROR;
            result.controlReason = "CONTROL_TOGGLE_READ_UNCONFIRMED"; return result;
        }
        const bool wasHigh = coil ? (result.data[3] & 1U) != 0 :
            result.data[3] != 0 || result.data[4] != 0;
        value = wasHigh ? 0 : coil ? 0xff00 : 1;
    }
    uint8_t request[11] = {target.slave, function, static_cast<uint8_t>(address >> 8),
        static_cast<uint8_t>(address), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value), 0, 0, 0, 0, 0};
    const size_t length = function == 16 ? 11 : 8;
    if (function == 16) {
        request[4] = 0; request[5] = 1; request[6] = 2;
        request[7] = static_cast<uint8_t>(value >> 8); request[8] = static_cast<uint8_t>(value);
    }
    const uint16_t crc = calculateCRC(request, length - 2);
    request[length - 2] = static_cast<uint8_t>(crc); request[length - 1] = static_cast<uint8_t>(crc >> 8);
    _ordinaryWriteAttempted = false;
    _ordinaryWriteTicket = &ticket;
    isOneShotReading = true;
    try { result = _executeModbusTransaction(request, length, target.slave, timeout, 0, true); }
    catch (...) { result.error = ONESHOT_BUSY; }
    isOneShotReading = false;
    _ordinaryWriteTicket = nullptr;
    result.outputAttempted = _ordinaryWriteAttempted;
    if (result.error == ONESHOT_SUCCESS) {
        bool exact = result.count == 8;
        // FC16 echoes slave/function/address/quantity, not the written value.
        // The CRC was checked by the real transaction; FC06/05 echo all bytes.
        const size_t echoLength = function == 16 ? 6 : 8;
        for (size_t i = 0; exact && i < echoLength; ++i) exact = result.data[i] == request[i];
        if (!exact) { result.error = ONESHOT_CRC_ERROR; result.delayReceipt = {}; }
    }
    result.controlReason = result.error == ONESHOT_SUCCESS ? "CONTROL_ACKNOWLEDGED_TRANSPORT_ONLY" :
        result.outputAttempted ? "CONTROL_RESULT_UNKNOWN" : "CONTROL_NOT_ATTEMPTED";
    return result;
}
#endif
