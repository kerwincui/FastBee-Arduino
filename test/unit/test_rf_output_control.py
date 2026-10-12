"""Actual RF PM + shared ordinary policy + checked GPIO SDK, and CALL decoding.

SDK faults/delays are deterministic seams. No RF receiver or physical load is
identified by these tests, and no firmware or device operation is performed.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'test/unit/cpp'


class RfOutputControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson are required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-rf-output-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        common = (ROOT / 'src/core/PeripheralControlArbitration.cpp').read_text(encoding='utf-8')
        actual = '\n'.join(cpp_function(common, signature) for signature in (
            'bool ordinaryTime(', 'bool ordinaryActorCurrent(', 'void fillResult(',
            'bool PeripheralManager::beginOrdinaryControl(',
            'bool PeripheralManager::ordinaryPermitCurrentLocked(',
            'bool PeripheralManager::retireOrdinaryDigitalLocked(',
            'void PeripheralManager::updateOrdinaryControlResultLocked(',
            'bool PeripheralManager::completeOrdinaryControlLocked(',
            'bool PeripheralManager::inspectOrdinaryControlLocked(',
            'bool PeripheralManager::commitOrdinaryControlLocked(',
            'void PeripheralManager::finishOrdinaryControl('))
        rf = (ROOT / 'src/core/PeripheralManagerRf.cpp').read_text(encoding='utf-8')
        actual += '\n' + '\n'.join(line for line in rf.splitlines() if not line.startswith('#include'))
        diagnostics = (ROOT / 'src/core/PeripheralManagerDiagnostics.cpp').read_text(encoding='utf-8')
        actual += '\n' + cpp_function(diagnostics, 'bool PeripheralManager::readRfLevel(')
        dispatch = (ROOT / 'include/core/NativeOutputDispatch.h').read_text(encoding='utf-8')
        helpers = '\n'.join(cpp_function(dispatch, signature) for signature in (
            'inline void rememberOrdinaryControlResult(', 'inline bool rejectOrdinaryDigitalAction('))
        action = (ROOT / 'include/core/RfOutputAction.h').read_text(encoding='utf-8')
        action = '\n'.join(line for line in action.splitlines() if not line.startswith(('#include', '#pragma')))
        template = (CPP / 'rf_output_harness.cpp.in').read_text(encoding='utf-8')
        source = directory / 'rf.cpp'
        source.write_text(template.replace('// @dispatch_helpers@', 'namespace FastBee {\n' + helpers + '\n}')
            .replace('// @production@', actual).replace('// @action_boundary@', action), encoding='utf-8')
        cls.binary = directory / 'rf.exe'
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-declarations', '-DARDUINO_ISR_FLAG=0', '-DCONFIG_IDF_TARGET_ESP32=1',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(CPP / 'managed_pin_mocks'),
            '-I' + str(CPP), '-I' + str(ROOT / 'include'), '-I' + str(headers[0].parent),
            str(source), str(ROOT / 'src/core/ManagedPinIo.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, encoding='utf-8', timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('decode', 'parameters', 'no_context', 'setup_failed', 'setup_partial', 'setup_collision',
              'active_high', 'active_low', 'release_active_low', 'release_failed', 'source_uart',
              'source_rule', 'source_config', 'source_screen', 'expiry', 'clock_failure',
              'active_write_failed', 'inactive_write_failed', 'cleanup_failed', 'conflict',
              'dedicated', 'not_ready', 'call_plain', 'call_json', 'call_template', 'call_invalid',
              'counter_saturates', 'tx_read_unavailable', 'zero_frame',
              'operation_deadline', 'operation_deadline_zero', 'operation_deadline_wrap',
              'operation_deadline_final', 'operation_deadline_cleanup'):
    setattr(RfOutputControlTests, 'test_' + _case, lambda self, case=_case: self.check(case))
