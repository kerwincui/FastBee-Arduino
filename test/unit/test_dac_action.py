"""Actual scalar decoder/locked writer -> real managed DAC backend; no devices.

The ordinary context admission is an explicit fixture seam; full production
lease admission is covered by test_ordinary_control, and authenticated HTTP
field handling by test_http_output_control. This suite keeps real ManagedPinIo
SDK/fault retention coverage on ESP32 and unsupported-chip coverage on S3/C3/C6.
Legacy writeData/writeDacAtVersion are asserted closed, never used as the new
positive path. Manual cases exercise the shared scalar decoder, not HTTP auth.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'test/unit/cpp'


class DacActionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        executor = (ROOT / 'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        execute = definition(executor, 'bool PeriphExecExecutor::executePeripheralAction(')
        branch = definition(execute, 'if (digitalTargetFound && FastBee::isOrdinaryScalarTarget(digitalTarget)) {')
        io = definition((ROOT / 'src/core/PeripheralManagerIo.cpp').read_text(encoding='utf-8'),
                        'bool PeripheralManager::writeData(')
        io_branch = io[io.index('case PeripheralType::DAC:'):io.index('// UART 发送')]
        gpio = (ROOT / 'src/core/PeripheralManagerGpio.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(definition(gpio, name) for name in (
            'bool PeripheralManager::writeManagedDac(',
            'bool PeripheralManager::releaseManagedDac('))
        scalar_write = definition((ROOT / 'src/core/PeripheralControlArbitration.cpp').read_text(encoding='utf-8'),
                                  'bool PeripheralManager::writeOrdinaryScalarLocked(')
        dispatch = (ROOT / 'include/core/NativeOutputDispatch.h').read_text(encoding='utf-8')
        helpers = '\n'.join(definition(dispatch, signature) for signature in (
            'inline bool isOrdinaryScalarTarget(', 'inline bool ordinaryScalarMaximum(',
            'inline bool ordinaryScalarHighValue(',
            'inline bool parseOrdinaryScalarValue(', 'inline void rememberOrdinaryControlResult(',
            'inline bool rejectOrdinaryDigitalAction(', 'inline bool dispatchOrdinaryScalarAction(',
            'inline bool parseOrdinaryBlinkInterval(', 'inline bool parseOrdinaryBreathePeriod(',
            'inline bool parseOrdinaryScalarCommand('))
        mapping = definition((ROOT / 'include/core/PeripheralConfig.h').read_text(encoding='utf-8'),
                             'inline uint32_t mapAnalogOutputPercent(')
        admission = definition((ROOT / 'src/core/DacWriteAccess.cpp').read_text(encoding='utf-8'), 'bool writeDacAtVersion(')
        template = (CPP / 'dac_action_harness.cpp.in').read_text(encoding='utf-8')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-dac-action-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        source = directory / 'dac_action.cpp'
        source.write_text(template.replace('// @executor@', branch)
                          .replace('// @write_data@', io_branch)
                          .replace('// @methods@', methods)
                          .replace('// @scalar_helpers@', helpers)
                          .replace('// @scalar_write@', scalar_write)
                          .replace('// @scalar_mapping@', mapping)
                          .replace('// @admission@', admission), encoding='utf-8')
        cls.binaries = {}
        for chip in ('ESP32', 'ESP32S3', 'ESP32C3', 'ESP32C6'):
            binary = directory / (chip + '.exe')
            built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-DCONFIG_IDF_TARGET_' + chip + '=1', '-I' + str(CPP),
                '-I' + str(CPP / 'managed_pin_mocks'), '-I' + str(ROOT / 'include'),
                str(source), str(ROOT / 'src/core/ManagedPinIo.cpp'), '-o', str(binary)],
                capture_output=True, text=True, timeout=90)
            if built.returncode:
                raise AssertionError(chip + '\n' + built.stdout + built.stderr)
            cls.binaries[chip] = binary

    def check(self, case):
        for chip, binary in self.binaries.items():
            with self.subTest(chip=chip):
                run = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=15)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


for _case in ('parser', 'valid', 'invalid_value', 'disabled', 'wrong_type', 'missing',
              'missing_runtime', 'uninitialized', 'runtime_error', 'suspended', 'cleanup_pending',
              'stale_uart', 'busy', 'no_mutex', 'adc_busy', 'foreign_pin', 'foreign_owner',
              'sdk_new_failure', 'sdk_partial_new_failure', 'sdk_write_failure',
              'manual_valid', 'manual_invalid', 'manual_changed', 'manual_replaced',
              'manual_uninitialized', 'manual_busy', 'legacy_bypass', 'permit_invalid'):
    setattr(DacActionTests, 'test_' + _case, lambda self, case=_case: self.check(case))


if __name__ == '__main__':
    unittest.main()
