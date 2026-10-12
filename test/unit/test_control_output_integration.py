"""Prepared checks for the mutation paths outside the isolated GPIO harness."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[2]


def body(text, signature):
    start=text.index(signature)
    opening=text.index('{',start)
    depth=0
    for index in range(opening,len(text)):
        if text[index]=='{':
            depth+=1
        elif text[index]=='}':
            depth-=1
            if not depth:
                return text[opening:index+1]
    raise AssertionError('Unterminated production function: '+signature)


class ControlOutputIntegrationTests(unittest.TestCase):
    def test_retirement_is_in_regular_maintenance(self):
        source=(ROOT/'src/core/PeripheralManagerDiagnostics.cpp').read_text(encoding='utf-8')
        maintenance=body(source,'void PeripheralManager::performMaintenance(')
        self.assertIn('retireControlOutputs();',maintenance)

    def test_crud_and_hardware_routes_guard_before_work(self):
        source=(ROOT/'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        for signature in (
            'bool PeripheralManager::updatePeripheral(const String& id, const PeripheralConfig& config, String& errorMsg)',
            'bool PeripheralManager::removePeripheral(', 'bool PeripheralManager::enablePeripheral(',
            'bool PeripheralManager::disablePeripheral(', 'bool PeripheralManager::initHardware(',
            'bool PeripheralManager::deinitHardware(', 'bool PeripheralManager::setupHardware(',
            'bool PeripheralManager::teardownHardware(', 'bool PeripheralManager::writeGpioStateLocked(',
            'bool PeripheralManager::startActionTicker('):
            with self.subTest(signature=signature):
                function=body(source,signature)
                if signature == 'bool PeripheralManager::setupHardware(':
                    # Both independent refusals precede hardware dispatch. A
                    # retained NeoPixel pin guard may report an error first.
                    admission=function[:function.index('    if (config.type == PeripheralType::PCA9685_PWM)')]
                    self.assertIn('fastbeeNeoPixelProjectPinRetained(', admission)
                    self.assertIn('if (controlOutputIsClaimed(config.id)) return false;', admission)
                    for operation in ('setupGPIOPin(', 'syncIoExpander(', 'ScreenDevice::instance().configure('):
                        self.assertLess(function.index('controlOutputIsClaimed('), function.index(operation))
                else:
                    prefix=function.splitlines()[:4]
                    self.assertTrue(any('if (controlOutputIsClaimed(' in line for line in prefix))

    def test_reload_cannot_clear_live_claims(self):
        source=(ROOT/'src/core/PeripheralManagerPersistence.cpp').read_text(encoding='utf-8')
        load=body(source,'bool PeripheralManager::loadConfiguration(')
        self.assertLess(load.index('configurationLock.isLocked()'),load.index('controlOutputIsClaimed()'))
        self.assertLess(load.index('controlOutputIsClaimed()'),load.index('peripherals.clear()'))

    def test_raw_script_outputs_guard_under_manager_lock(self):
        source=(ROOT/'src/core/ScriptEngine.cpp').read_text(encoding='utf-8')
        execute=body(source,'bool ScriptEngine::execute(')
        for command,write in (('CMD_GPIO','pm.writeScriptGpio('),('CMD_PWM','pm.writeScriptPwm('),('CMD_DAC','pm.writeScriptDac(')):
            branch=body(execute,'case ScriptCmdType::'+command+':')
            self.assertLess(branch.index('RecursiveMutexGuard controlGuard'),branch.index('controlOutputPinIsClaimed'))
            self.assertLess(branch.index('controlOutputPinIsClaimed'),branch.index(write))
            prepared=branch.index('pm.prepareScriptPin(')
            self.assertLess(prepared,branch.index('!sourceCurrent()',prepared))
            self.assertLess(branch.index('!sourceCurrent()',prepared),branch.index(write))

if __name__=='__main__':
    unittest.main()
