/**
 * @description: PeripheralManager Modbus delegation implementation
 */

#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "systems/LoggerSystem.h"

namespace {
struct ModbusMotorLimitResult {
    bool enabled = false;
    bool allowed = true;
    bool isMove = false;
    uint16_t pulse = 0;
    int32_t nextPosition = 0;
};

ModbusMotorLimitResult evaluateModbusMotorLimit(const PeripheralConfig& config,
                                                 uint16_t regAddr,
                                                 uint16_t value) {
    ModbusMotorLimitResult result;
    result.nextPosition = config.params.modbus.motorCurrentPosition;
    if (config.params.modbus.deviceType != 3 ||
        config.params.modbus.motorMaxPosition <= config.params.modbus.motorMinPosition) {
        return result;
    }

    const bool forward = regAddr == config.params.modbus.motorRegs[0];
    const bool reverse = regAddr == config.params.modbus.motorRegs[1];
    if (!forward && !reverse) return result;

    result.enabled = true;
    result.isMove = true;
    int32_t desiredPulse = value > 1 ? value : 0;
    if (desiredPulse <= 0 && config.params.modbus.motorLastPulse > 0) {
        desiredPulse = config.params.modbus.motorLastPulse;
    }
    if (desiredPulse <= 0 && config.params.modbus.motorMoveStep > 0) {
        desiredPulse = config.params.modbus.motorMoveStep;
    }
    if (desiredPulse <= 0) {
        result.allowed = false;
        return result;
    }

    int32_t room = forward
        ? config.params.modbus.motorMaxPosition - config.params.modbus.motorCurrentPosition
        : config.params.modbus.motorCurrentPosition - config.params.modbus.motorMinPosition;
    if (room <= 0) {
        result.allowed = false;
        return result;
    }

    if (desiredPulse > room) desiredPulse = room;
    if (desiredPulse > 65535) desiredPulse = 65535;
    result.pulse = static_cast<uint16_t>(desiredPulse);
    result.nextPosition = forward
        ? config.params.modbus.motorCurrentPosition + desiredPulse
        : config.params.modbus.motorCurrentPosition - desiredPulse;
    return result;
}
}

void PeripheralManager::setModbusCallbacks(ModbusCoilWriteFunc coilWrite,
                                            ModbusRegWriteFunc regWrite) {
    RecursiveMutexGuard lock(_mutex);
    _modbusCoilWrite = coilWrite;
    _modbusRegWrite = regWrite;
    LOG_INFO("Peripheral Manager: Modbus write callbacks registered");
}

void PeripheralManager::clearModbusCallbacks() {
    RecursiveMutexGuard lock(_mutex);
    _modbusCoilWrite = nullptr;
    _modbusRegWrite = nullptr;
    LOG_INFO("Peripheral Manager: Modbus write callbacks cleared");
}

bool PeripheralManager::writeModbusPin(const String& id,
                                       const PeripheralConfig& config,
                                       GPIOState state) {
    if (!_modbusCoilWrite && !_modbusRegWrite) {
        LOG_WARNING("Peripheral Manager: Modbus write callbacks not set");
        return false;
    }

    bool value = state == GPIOState::STATE_HIGH;
    if (config.params.modbus.ncMode) value = !value;

    const uint16_t addr = config.params.modbus.coilBase;
    bool success = false;
    if (config.params.modbus.controlProtocol == 0) {
        if (_modbusCoilWrite) {
            success = _modbusCoilWrite(config.params.modbus.slaveAddress, addr, value);
        }
    } else if (_modbusRegWrite) {
        const uint16_t regVal = value ? 0xFF00 : 0x0000;
        success = _modbusRegWrite(config.params.modbus.slaveAddress, addr, regVal);
    }

    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].state.gpio.currentState = state;
        runtimeStates[id].lastActivity = millis();
    }

    LOG_INFOF("Peripheral Manager: Modbus writePin '%s' slave=%d addr=%d state=%s %s",
              id.c_str(), config.params.modbus.slaveAddress, addr,
              state == GPIOState::STATE_HIGH ? "HIGH" : "LOW",
              success ? "OK" : "FAIL");
    return success;
}

bool PeripheralManager::writeModbusPWM(const String& id,
                                       const PeripheralConfig& config,
                                       uint32_t dutyCycle) {
    if (!_modbusRegWrite) {
        LOG_WARNING("Peripheral Manager: Modbus register write callback not set");
        return false;
    }
    if (config.params.modbus.deviceType != 1) {
        LOG_WARNINGF("Peripheral Manager: '%s' is not a PWM Modbus device", id.c_str());
        return false;
    }

    const uint16_t regAddr = config.params.modbus.pwmRegBase;
    const uint16_t regVal = static_cast<uint16_t>(dutyCycle);
    const bool success = _modbusRegWrite(config.params.modbus.slaveAddress, regAddr, regVal);
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
    }

    LOG_INFOF("Peripheral Manager: Modbus writePWM '%s' slave=%d reg=%d duty=%d %s",
              id.c_str(), config.params.modbus.slaveAddress, regAddr,
              static_cast<int>(dutyCycle), success ? "OK" : "FAIL");
    return success;
}

bool PeripheralManager::writeModbusCoil(const String& id, uint16_t coilAddr, bool value) {
    RecursiveMutexGuard lock(_mutex);
    if (!_modbusCoilWrite) {
        LOG_WARNING("Peripheral Manager: Modbus coil write callback not set");
        return false;
    }

    auto config = getPeripheral(id);
    if (!config || !config->isModbusPeripheral()) {
        LOG_WARNINGF("Peripheral Manager: '%s' is not a Modbus peripheral", id.c_str());
        return false;
    }

    const bool success = _modbusCoilWrite(config->params.modbus.slaveAddress, coilAddr, value);
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
    }

    LOG_INFOF("Peripheral Manager: Modbus writeCoil '%s' slave=%d coil=%d val=%d %s",
              id.c_str(), config->params.modbus.slaveAddress, coilAddr, value,
              success ? "OK" : "FAIL");
    return success;
}

bool PeripheralManager::writeModbusReg(const String& id, uint16_t regAddr, uint16_t value) {
    RecursiveMutexGuard lock(_mutex);
    if (!_modbusRegWrite) {
        LOG_WARNING("Peripheral Manager: Modbus register write callback not set");
        return false;
    }

    auto config = getPeripheral(id);
    if (!config || !config->isModbusPeripheral()) {
        LOG_WARNINGF("Peripheral Manager: '%s' is not a Modbus peripheral", id.c_str());
        return false;
    }

    ModbusMotorLimitResult motorLimit = evaluateModbusMotorLimit(*config, regAddr, value);
    if (motorLimit.enabled && !motorLimit.allowed) {
        LOG_WARNINGF("Peripheral Manager: Modbus motor '%s' blocked by soft limit (pos=%ld min=%ld max=%ld reg=%u)",
                     id.c_str(),
                     static_cast<long>(config->params.modbus.motorCurrentPosition),
                     static_cast<long>(config->params.modbus.motorMinPosition),
                     static_cast<long>(config->params.modbus.motorMaxPosition),
                     static_cast<unsigned int>(regAddr));
        return false;
    }
    if (motorLimit.enabled && motorLimit.isMove && config->params.modbus.motorRegs[4] != 0) {
        const bool pulseOk = _modbusRegWrite(config->params.modbus.slaveAddress,
                                             config->params.modbus.motorRegs[4],
                                             motorLimit.pulse);
        if (!pulseOk) {
            LOG_WARNINGF("Peripheral Manager: Modbus motor '%s' failed to write bounded pulse=%u",
                         id.c_str(), static_cast<unsigned int>(motorLimit.pulse));
            return false;
        }
    }

    const bool success = _modbusRegWrite(config->params.modbus.slaveAddress, regAddr, value);
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        runtimeStates[id].lastActivity = millis();
    }
    if (success && config->params.modbus.deviceType == 3) {
        if (regAddr == config->params.modbus.motorRegs[4]) {
            config->params.modbus.motorLastPulse = value;
        } else if (motorLimit.enabled && motorLimit.isMove) {
            config->params.modbus.motorCurrentPosition = motorLimit.nextPosition;
            config->params.modbus.motorLastPulse = motorLimit.pulse;
        }
    }

    LOG_INFOF("Peripheral Manager: Modbus writeReg '%s' slave=%d reg=%d val=%d %s",
              id.c_str(), config->params.modbus.slaveAddress, regAddr, value,
              success ? "OK" : "FAIL");
    return success;
}
