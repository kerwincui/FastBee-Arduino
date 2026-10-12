"""Actual synchronous TM1637 bytes, PM arbitration and typed/script action branches.
ACK and lock faults are deterministic doubles; no physical display verification.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'test/unit/cpp'


class DisplayOrdinaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        common = (ROOT / 'src/core/PeripheralControlArbitration.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(cpp_function(common, signature) for signature in (
            'bool ordinaryTime(', 'bool ordinaryActorCurrent(', 'void fillResult(',
            'bool PeripheralManager::beginOrdinaryControl(',
            'bool PeripheralManager::ordinaryPermitCurrentLocked(',
            'bool PeripheralManager::retireOrdinaryDigitalLocked(',
            'void PeripheralManager::updateOrdinaryControlResultLocked(',
            'bool PeripheralManager::completeOrdinaryControlLocked(',
            'bool PeripheralManager::inspectOrdinaryControlLocked(',
            'bool PeripheralManager::commitOrdinaryControlLocked(',
            'void PeripheralManager::finishOrdinaryControl(',
            'void PeripheralManager::serviceOrdinaryControlExpiry('))
        display = (ROOT / 'src/core/PeripheralManagerDisplay.cpp').read_text(encoding='utf-8')
        methods_display = '\n'.join(cpp_function(display, signature) for signature in (
            'bool PeripheralManager::submitOrdinaryDisplay(',
            'bool PeripheralManager::ordinaryDisplayQuiescentLocked(',
            'bool PeripheralManager::submitOrdinaryLcd(',
            'bool PeripheralManager::getActiveLcdTarget(',
            'void PeripheralManager::performLcdMaintenance('))
        driver = (ROOT / 'src/peripherals/SevenSegmentDriver.cpp').read_text(encoding='utf-8')
        driver = driver[driver.index('// TM1637 指令'):driver.rindex('#endif')]
        header = (ROOT / 'include/peripherals/SevenSegmentDriver.h').read_text(encoding='utf-8')
        driver_type = cpp_function(header, 'class SevenSegmentDriver {') + ';'
        dispatch = (ROOT / 'include/core/NativeOutputDispatch.h').read_text(encoding='utf-8')
        helpers = 'namespace FastBee {\n' + '\n'.join(cpp_function(dispatch, signature) for signature in (
            'inline void rememberOrdinaryControlResult(',
            'inline bool rejectOrdinaryDigitalAction(')) + '\n}\n'
        executor = (ROOT / 'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        typed = '\n'.join(cpp_function(executor, 'case ExecActionType::' + name + ': {')
            for name in ('ACTION_DISPLAY_NUMBER', 'ACTION_DISPLAY_TEXT', 'ACTION_DISPLAY_CLEAR', 'ACTION_OLED_DISPLAY'))
        script = (ROOT / 'src/core/ScriptEngine.cpp').read_text(encoding='utf-8')
        lcd_text = cpp_function(script, 'else if ((cmd.subAction == "TEXT" || cmd.subAction == "DISPLAY")')
        lcd_start = script.index('                if (cmd.subAction == "SHOW" && isLcdPeripheral(cmd.strParam))')
        lcd_actions = script[lcd_start:script.index('\n#endif', lcd_start)]
        script = cpp_function(script, 'if (pm.getPeripheralSnapshot(cmd.strParam, nativeTarget) &&\n'
            '                    nativeTarget.type == PeripheralType::SEVEN_SEGMENT_TM1637) {')
        support = cpp_function(common, 'bool PeripheralManager::ordinaryBackendSupportedLocked(')
        support_start = support.index('    if (config.type == PeripheralType::LCD)')
        tm_start = support.index('    if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637)')
        support_end = support.index(';', tm_start) + 1
        quiescence = cpp_function(common, 'bool PeripheralManager::ordinaryBackendQuiescentLocked(')
        quiet_start = quiescence.index('    if (config != peripherals.end() && (config->second.type == PeripheralType::SEVEN_SEGMENT_TM1637')
        quiet_end = quiescence.index(';', quiet_start) + 1
        code = (CPP / 'display_ordinary_harness.cpp.in').read_text(encoding='utf-8')
        for marker, content in (
            ('driver_type', driver_type), ('driver_production', driver),
            ('helpers', helpers), ('common_production', methods), ('display_production', methods_display),
            ('typed', typed), ('script', script), ('lcd_text', lcd_text), ('lcd_actions', lcd_actions),
            ('display_support', support[support_start:support_end]),
            ('display_quiescence', quiescence[quiet_start:quiet_end])):
            code = code.replace('// @' + marker + '@', content)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-display-ordinary-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        source = folder / 'display.cpp'
        source.write_text(code, encoding='utf-8')
        cls.binary = folder / 'display.exe'
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
            str(source), '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('lcd_success', 'lcd_conflict', 'lcd_driver_failure', 'lcd_identity', 'lcd_parameters',
              'lcd_maintenance', 'lcd_expiry', 'lcd_expiry_busy', 'lcd_expiry_driver_busy', 'lcd_typed', 'lcd_script', 'lcd_font', 'lcd_target', 'success', 'init_nack', 'no_context', 'parameters', 'disabled', 'identity',
              'dedicated', 'pm_busy', 'driver_busy', 'stale_config', 'stale_rule', 'stale_uart',
              'stale_screen', 'brightness', 'release_nack', 'typed', 'script', 'conflict',
              'replacement', 'override_denied', 'expiry'):
    setattr(DisplayOrdinaryTests, 'test_' + _case, lambda self, case=_case: self.check(case))
for _byte in range(1, 8):
    setattr(DisplayOrdinaryTests, 'test_nack_' + str(_byte),
            lambda self, byte=_byte: self.check('nack_' + str(byte)))
